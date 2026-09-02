// ---------------------------------------------------------------------------
// indexed_plane_smoke (#271) — el pipeline INDEXADO se valida contra el
// renderer del core, BIT A BIT, sobre frames reales de una toma.
//
// Por cada frame:
//   · esperado: ayther_recompose_frame con la máscara de capas en "solo plano
//     B" (override R-2 del fork) — el MISMO renderer que midió el spike R-1
//     recompone «plano B sobre backdrop» en RGB565 → se expande a 888 con la
//     conversión única (VkIndexedPlane::genesis_color_rgba es la misma tabla).
//   · GPU: clear al color de backdrop + VkIndexedPlane::draw_cells con las
//     celdas del nametable B (walk CPU con scroll de FrameView) → readback.
//   · comparación: byte a byte BGRA. Cualquier diferencia = FAIL (+ PNGs).
//
// Recorre varios frames de la MISMA sesión → también ejercita la subida
// incremental (frame 2 en adelante sólo suben los tiles que cambiaron).
//
// Requiere GPU + el fork con el override de capas. Frames elegidos con
// hscroll full-screen (GA usa full toda la demo — medido en R-1): el pase de
// R-2 modela scroll whole-plane; el raster por línea es de R-3 en adelante.
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target indexed_plane_smoke
//   Args:  <rec.arp> [frame ...]   (default: 300 900 1500 2000)
//   Env:   AYTHER_PROBE_ROM = ROM del juego (el core sale de test_config.toml)
// ---------------------------------------------------------------------------
#include "ayther_env.h"
#include "ayther_session.h"
#include "ayther_recording.h"
#include "../../tests/support/vulkan_test_context.h"
#include "vulkan_backend/vk_render_target.h"
#include "vulkan_backend/vk_indexed_plane.h"
#include <SDL3/SDL.h>
#include <stb_image_write.h>   // impl linkeada desde el engine (tile_tex_cache)

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#ifndef AYTHER_SOURCE_DIR
#define AYTHER_SOURCE_DIR "."
#endif
using ayther::FrameView;

// Flags de ayther_recompose_frame (espejo de vdp_render.h del fork).
static constexpr unsigned kNoSh        = 0x01u;
static constexpr unsigned kLayerB      = 0x02u;   // AYTHER_LAYER_B
static constexpr unsigned kLayerShift  = 24;      // AYTHER_RC_LAYER_MASK(m) = m<<24
using RecomposeFn = int (*)(uint16_t*, int, unsigned, int*, int*);

static std::string toml_quoted(const std::string& l) {
    const auto a = l.find('"'), b = l.rfind('"');
    return (a == std::string::npos || b <= a) ? std::string() : l.substr(a + 1, b - a - 1);
}
static std::string resolve(const std::string& p, const std::string& base) {
    if (p.empty() || (p.size() > 1 && p[1] == ':') || p[0] == '/' || p[0] == '\\') return p;
    return base + "/" + p;
}

// ---- readback mínimo del smoke (molde de AytherRenderer::readback_*, pero
//      con memoria Vulkan pelada — VMA queda privado del engine) -------------
struct SmokeReadback {
    VkBuffer       buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void*          map = nullptr;

    bool init(VulkanTestContext& ctx, VkDeviceSize size) {
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size  = size;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(ctx.device(), &bi, nullptr, &buf) != VK_SUCCESS) return false;
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(ctx.device(), buf, &req);
        VkPhysicalDeviceMemoryProperties props{};
        vkGetPhysicalDeviceMemoryProperties(ctx.physical_device(), &props);
        uint32_t type = UINT32_MAX;
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
            if ((req.memoryTypeBits & (1u << i)) &&
                (props.memoryTypes[i].propertyFlags & want) == want) { type = i; break; }
        if (type == UINT32_MAX) return false;
        VkMemoryAllocateInfo ai{};
        ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize  = req.size;
        ai.memoryTypeIndex = type;
        if (vkAllocateMemory(ctx.device(), &ai, nullptr, &mem) != VK_SUCCESS) return false;
        if (vkBindBufferMemory(ctx.device(), buf, mem, 0) != VK_SUCCESS) return false;
        return vkMapMemory(ctx.device(), mem, 0, VK_WHOLE_SIZE, 0, &map) == VK_SUCCESS;
    }
    void shutdown(VulkanTestContext& ctx) {
        if (buf) vkDestroyBuffer(ctx.device(), buf, nullptr);
        if (mem) { vkUnmapMemory(ctx.device(), mem); vkFreeMemory(ctx.device(), mem, nullptr); }
        buf = VK_NULL_HANDLE; mem = VK_NULL_HANDLE; map = nullptr;
    }
};

static void barrier(VkCommandBuffer cmd, VkImage img,
                    VkImageLayout from, VkImageLayout to,
                    VkAccessFlags sa, VkAccessFlags da,
                    VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
    VkImageMemoryBarrier b{};
    b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout           = from;
    b.newLayout           = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image               = img;
    b.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    b.srcAccessMask       = sa;
    b.dstAccessMask       = da;
    vkCmdPipelineBarrier(cmd, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// Un frame completo: uploads incrementales + clear a backdrop + draw + copia
// al buffer host. Devuelve el BGRA mapeado (válido hasta el próximo frame).
static const uint8_t* render_and_read(
        VulkanTestContext& ctx, VkRenderTarget& target, VkCommandBuffer cmd, VkFence fence,
        SmokeReadback& rb, VkIndexedPlane& plane,
        const VkIndexedPlane::CellQuad* cells, size_t count,
        const uint8_t* vram, size_t vsz, const uint8_t* cram, size_t csz,
        float br, float bg, float bb, int32_t scissor_x) {
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) return nullptr;

    plane.upload_vram(ctx, cmd, vram, vsz);
    plane.upload_cram(ctx, cmd, cram, csz);

    // El contenido anterior se descarta (UNDEFINED) — el clear repinta todo.
    barrier(cmd, target.image(), VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue cc{};
    cc.float32[0] = br; cc.float32[1] = bg; cc.float32[2] = bb; cc.float32[3] = 1.f;
    VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cmd, target.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &cc, 1, &range);
    barrier(cmd, target.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    // El pass del plano entra en COLOR_ATTACHMENT y sale en TRANSFER_DST.
    plane.draw_cells(ctx, cmd, cells, count, target.extent().width,
                     target.extent().height, scissor_x);

    barrier(cmd, target.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy r{};
    r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    r.imageExtent      = { target.extent().width, target.extent().height, 1 };
    vkCmdCopyImageToBuffer(cmd, target.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           rb.buf, 1, &r);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{};
    si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cmd;
    if (vkQueueSubmit(ctx.graphics_queue(), 1, &si, fence) != VK_SUCCESS) return nullptr;
    vkWaitForFences(ctx.device(), 1, &fence, VK_TRUE, UINT64_MAX);
    vkResetFences(ctx.device(), 1, &fence);
    return static_cast<const uint8_t*>(rb.map);
}

static int g_checks = 0, g_fails = 0;
static void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) ++g_fails;
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
}

// RGB565 (recompose) → bytes BGRA con la MISMA expansión que la paleta GPU
// (replicación de bits altos — genesis_color_rgba parte del mismo 565).
static void expand565_bgra(uint16_t p, uint8_t* d) {
    const unsigned r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
    d[0] = (uint8_t)((b << 3) | (b >> 2));
    d[1] = (uint8_t)((g << 2) | (g >> 4));
    d[2] = (uint8_t)((r << 3) | (r >> 2));
    d[3] = 0xFF;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "uso: indexed_plane_smoke <rec.arp> [frame ...]\n");
        return 2;
    }
    const std::string rec_path = argv[1];
    std::vector<uint32_t> frames;
    for (int i = 2; i < argc; ++i)
        frames.push_back((uint32_t)std::strtoul(argv[i], nullptr, 10));
    if (frames.empty()) frames = { 300, 900, 1500, 2000 };

    const std::string root = AYTHER_SOURCE_DIR;
    std::string core, rom, line;
    {
        std::ifstream cfg(root + "/tests/test_config.toml");
        while (std::getline(cfg, line))
            if (line.find("core") != std::string::npos &&
                line.find('=') != std::string::npos && core.empty())
                core = toml_quoted(line);
    }
    core = resolve(core, root);
    if (const char* er = ayther::env_get("AYTHER_PROBE_ROM")) rom = er;
    if (rom.empty()) { std::fprintf(stderr, "[FAIL] falta AYTHER_PROBE_ROM\n"); return 2; }

    // ---- sesión + toma -----------------------------------------------------
    ayther::AytherSession::Config c;
    c.core_path = core; c.rom_path = rom; c.enable_audio = false;
    auto r = ayther::AytherSession::create(c);
    if (!r) { std::fprintf(stderr, "[FAIL] create: %s\n", r.error.message.c_str()); return 1; }
    std::unique_ptr<ayther::AytherSession>& s = *r;
    auto rc = reinterpret_cast<RecomposeFn>(s->core_export("ayther_recompose_frame"));
    if (!rc) { std::fprintf(stderr, "[FAIL] el core no exporta ayther_recompose_frame\n"); return 1; }
    auto rec = ayther::AytherRecording::load(rec_path);
    if (!rec) { std::fprintf(stderr, "[FAIL] no se pudo cargar %s\n", rec_path.c_str()); return 1; }

    // ---- Vulkan: ventana oculta (VulkanTestContext no tiene camino headless) -------
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "[FAIL] SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* win = SDL_CreateWindow("indexed_plane_smoke", 64, 64,
                                       SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (!win) { std::fprintf(stderr, "[FAIL] SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
    VulkanTestContext ctx;
    if (!ctx.init(win)) { std::fprintf(stderr, "[FAIL] VulkanTestContext::init\n"); return 1; }
    std::printf("GPU: %s\n", ctx.gpu_name().c_str());

    // Primer frame para conocer las dimensiones del canvas. Si un frame
    // posterior cambia de modo (H40↔H32 — GA pasa a 256px en el título), el
    // smoke se re-arma a la resolución nueva y sigue.
    const FrameView* fv0 = s->replay_seek(*rec, frames[0]);
    if (!fv0 || !fv0->fb_width) { std::fprintf(stderr, "[FAIL] seek inicial\n"); return 1; }
    uint32_t W = fv0->fb_width, H = fv0->fb_height;

    VkRenderTarget target;
    if (!target.init(ctx, W, H)) { std::fprintf(stderr, "[FAIL] target %ux%u\n", W, H); return 1; }
    VkIndexedPlane plane;
    const std::string sh = root + "/shaders/";
    if (!plane.init(ctx, target, (sh + "indexed_plane.vert.spv").c_str(),
                    (sh + "indexed_plane.frag.spv").c_str())) {
        std::fprintf(stderr, "[FAIL] VkIndexedPlane::init\n");
        return 1;
    }

    // ---- readback propio (molde de AytherRenderer::readback_init) ----------
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    {
        VkCommandPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                   VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = ctx.graphics_family();
        vkCreateCommandPool(ctx.device(), &pi, nullptr, &pool);
        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(ctx.device(), &ai, &cmd);
        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        vkCreateFence(ctx.device(), &fi, nullptr, &fence);
    }
    SmokeReadback rb;
    if (!rb.init(ctx, (VkDeviceSize)W * H * 4)) {
        std::fprintf(stderr, "[FAIL] readback buffer\n");
        return 1;
    }

    std::vector<uint16_t> expected565((size_t)W * H);
    std::vector<uint8_t>  expected((size_t)W * H * 4);

    std::printf("=== indexed_plane_smoke (#271) ===  canvas %ux%u\n\n", W, H);

    for (uint32_t f : frames) {
        const FrameView* fv = s->replay_seek(*rec, f);
        if (!fv || !fv->fb_width) {
            std::printf("f%u:\n", f);
            check(false, "seek");
            continue;
        }
        if (fv->fb_width != W || fv->fb_height != H) {
            // Cambio de modo (H40↔H32): re-armar target + pase + readback.
            vkDeviceWaitIdle(ctx.device());
            W = fv->fb_width; H = fv->fb_height;
            rb.shutdown(ctx);
            plane.shutdown(ctx);
            if (!target.resize(ctx, W, H) ||
                !plane.init(ctx, target, (sh + "indexed_plane.vert.spv").c_str(),
                            (sh + "indexed_plane.frag.spv").c_str()) ||
                !rb.init(ctx, (VkDeviceSize)W * H * 4)) {
                std::printf("f%u:\n", f);
                check(false, "re-armado a la resolución nueva");
                continue;
            }
            expected565.resize((size_t)W * H);
            expected.resize((size_t)W * H * 4);
            std::printf("(canvas → %ux%u)\n", W, H);
        }
        size_t rsz = 0, vsz = 0, csz = 0;
        const uint8_t* regs = s->vdp_regs(&rsz);
        const uint8_t* vram = s->video_ram(&vsz);
        const uint8_t* cram = s->color_ram(&csz);
        if (!regs || rsz < 0x20 || !vram || vsz < 0x10000 || !cram || csz < 0x80) {
            std::printf("f%u:\n", f);
            check(false, "VRAM/CRAM/regs del fork presentes");
            continue;
        }
        std::printf("f%u: hscroll=%s B_hs=%d B_vs=%d\n", f,
                    (regs[11] & 3) == 0 ? "full" : "raster", (int)fv->plane_hscroll[1],
                    (int)fv->plane_vscroll[1]);

        // ---- esperado: el core recompone "solo plano B" -------------------
        int rw = 0, rh = 0;
        const unsigned flags = kNoSh | (kLayerB << kLayerShift);
        if (!rc(expected565.data(), (int)expected565.size(), flags, &rw, &rh) ||
            rw != (int)W || rh != (int)H) {
            check(false, "recompose(mask=B) del core");
            continue;
        }
        for (size_t i = 0; i < (size_t)W * H; ++i)
            expand565_bgra(expected565[i], &expected[i * 4]);

        // ---- celdas del nametable B (walk CPU, scroll whole-plane) --------
        auto cells_of = [](uint8_t bits) { return bits == 1 ? 64 : bits == 3 ? 128 : 32; };
        const int wc = cells_of(regs[0x10] & 3), hc = cells_of((regs[0x10] >> 4) & 3);
        const int wpx = wc * 8, hpx = hc * 8;
        const uint32_t base = (uint32_t)(regs[0x04] & 0x07) << 13;
        const int hs = fv->plane_hscroll[1], vs = fv->plane_vscroll[1];

        std::vector<VkIndexedPlane::CellQuad> cells;
        cells.reserve(2048);
        for (int cy = 0; cy < hc; ++cy) {
            // screen_y = plane_y − V (cam_y ≡ +V, validado en mode3_spike)
            int sy = (cy * 8 - vs) % hpx;
            if (sy < -7) sy += hpx;
            for (; sy < (int)H; sy += hpx) {
                if (sy <= -8) continue;
                for (int cx = 0; cx < wc; ++cx) {
                    // screen_x = plane_x + H (cam_x ≡ −H)
                    int sx = (cx * 8 + hs) % wpx;
                    if (sx < -7) sx += wpx;
                    for (int x = sx; x < (int)W; x += wpx) {
                        if (x <= -8) continue;
                        const uint32_t off = base + (uint32_t)(cy * wc + cx) * 2;
                        // u16 del bus 68k = lectura LE del buffer word-swapped
                        const uint16_t word = (uint16_t)(vram[off] | (vram[off + 1] << 8));
                        VkIndexedPlane::CellQuad q;
                        q.x       = (float)x;
                        q.y       = (float)sy;
                        q.pattern = (uint16_t)(word & 0x7FF);
                        q.flips   = (uint8_t)((word >> 11) & 3);
                        q.pal     = (uint8_t)((word >> 13) & 3);
                        cells.push_back(q);
                    }
                }
            }
        }

        // ---- GPU: clear a backdrop + draw + readback ----------------------
        const uint16_t bd_packed =
            (uint16_t)(cram[(regs[7] & 0x3F) * 2] | (cram[(regs[7] & 0x3F) * 2 + 1] << 8));
        const uint32_t bd = VkIndexedPlane::genesis_color_rgba(bd_packed);
        const int32_t scissor_x = (regs[0] & 0x20) ? 8 : 0;
        const uint8_t* got = render_and_read(
            ctx, target, cmd, fence, rb, plane, cells.data(), cells.size(),
            vram, vsz, cram, csz,
            (float)((bd >> 0) & 0xFF) / 255.f, (float)((bd >> 8) & 0xFF) / 255.f,
            (float)((bd >> 16) & 0xFF) / 255.f, scissor_x);
        if (!got) { check(false, "render + readback GPU"); continue; }

        // ---- comparación bit a bit ----------------------------------------
        size_t mismatch = 0;
        for (size_t i = 0; i < (size_t)W * H * 4; ++i)
            mismatch += (got[i] != expected[i]);
        char msg[128];
        std::snprintf(msg, sizeof(msg),
                      "plano B GPU == recompose del core (%zu celdas, %zu bytes distintos)",
                      cells.size(), mismatch);
        check(mismatch == 0, msg);
        if (mismatch) {
            std::vector<uint8_t> png((size_t)W * H * 4);
            for (size_t i = 0; i < (size_t)W * H; ++i) {   // BGRA→RGBA
                png[i * 4 + 0] = got[i * 4 + 2];
                png[i * 4 + 1] = got[i * 4 + 1];
                png[i * 4 + 2] = got[i * 4 + 0];
                png[i * 4 + 3] = 0xFF;
            }
            char name[64];
            std::snprintf(name, sizeof(name), "indexed_plane_f%u_got.png", f);
            stbi_write_png(name, (int)W, (int)H, 4, png.data(), (int)W * 4);
            for (size_t i = 0; i < (size_t)W * H; ++i) {
                png[i * 4 + 0] = expected[i * 4 + 2];
                png[i * 4 + 1] = expected[i * 4 + 1];
                png[i * 4 + 2] = expected[i * 4 + 0];
                png[i * 4 + 3] = 0xFF;
            }
            std::snprintf(name, sizeof(name), "indexed_plane_f%u_want.png", f);
            stbi_write_png(name, (int)W, (int)H, 4, png.data(), (int)W * 4);
            std::printf("        (dump: indexed_plane_f%u_{got,want}.png)\n", f);
        }
    }

    // ---- teardown ----------------------------------------------------------
    vkDeviceWaitIdle(ctx.device());
    rb.shutdown(ctx);
    plane.shutdown(ctx);
    target.shutdown(ctx);
    if (fence) vkDestroyFence(ctx.device(), fence, nullptr);
    if (pool)  vkDestroyCommandPool(ctx.device(), pool, nullptr);
    ctx.shutdown();
    SDL_DestroyWindow(win);
    SDL_Quit();

    std::printf("\n%d checks, %d fails\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
