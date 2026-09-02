// ---------------------------------------------------------------------------
// enhance_shader_smoke (#504) — ORÁCULO GPU del filtro «Mejorar por software»
// (#493): indexed_plane.frag con `attr & 16` contra la referencia de CPU
// xbr_ref.h, píxel a píxel, SIN ROM ni sesión (receta de video_shader_smoke:
// VkIndexedPlane directo sobre un VkRenderTarget, VRAM/CRAM sintéticas).
//
// Por caso: un tile 8×8 de índices (y su anillo de vecinos en VRAM, que el
// quad NO dibuja pero el shader lee por nb0..nb3) se dibuja a ×6 (48×48) con
// enhance=1 sobre un fondo conocido, y se compara con lo que la referencia
// dice que debe salir: (a) qué píxeles se escriben (discreto: exacto),
// (b) color con tolerancia de 2 LSB (la mezcla en paleta y fwidth() no son
// bit-exactos entre GPUs), (c) NO-VACUIDAD: cada caso tiene que mostrar el
// rasgo que prueba (mezcla parcial, alpha parcial, diferencia con/sin vecino).
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target enhance_shader_smoke (ctest label gpu)
// ---------------------------------------------------------------------------
#include "xbr_ref.h"
#include "../../tests/support/vulkan_test_context.h"
#include "vulkan_backend/vk_render_target.h"
#include "vulkan_backend/vk_indexed_plane.h"
#include <SDL3/SDL.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef AYTHER_SOURCE_DIR
#define AYTHER_SOURCE_DIR "."
#endif

static int g_checks = 0, g_fails = 0;
static void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) ++g_fails;
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
}

// ---- readback mínimo (molde de indexed_plane_smoke) -------------------------
struct SmokeReadback {
    VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; void* map = nullptr;
    bool init(VulkanTestContext& ctx, VkDeviceSize size) {
        VkBufferCreateInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = size; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(ctx.device(), &bi, nullptr, &buf) != VK_SUCCESS) return false;
        VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(ctx.device(), buf, &req);
        VkPhysicalDeviceMemoryProperties props{};
        vkGetPhysicalDeviceMemoryProperties(ctx.physical_device(), &props);
        uint32_t type = UINT32_MAX;
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
            if ((req.memoryTypeBits & (1u << i)) &&
                (props.memoryTypes[i].propertyFlags & want) == want) { type = i; break; }
        if (type == UINT32_MAX) return false;
        VkMemoryAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size; ai.memoryTypeIndex = type;
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

static void barrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to,
                    VkAccessFlags sa, VkAccessFlags da,
                    VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
    VkImageMemoryBarrier b{}; b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img; b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    b.srcAccessMask = sa; b.dstAccessMask = da;
    vkCmdPipelineBarrier(cmd, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
}

static const uint8_t* render_and_read(
        VulkanTestContext& ctx, VkRenderTarget& target, VkCommandBuffer cmd, VkFence fence,
        SmokeReadback& rb, VkIndexedPlane& plane,
        const VkIndexedPlane::CellQuad* cells, size_t count,
        const uint8_t* vram, size_t vsz, const uint8_t* cram, size_t csz,
        const float bg[3]) {
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) return nullptr;
    plane.upload_vram(ctx, cmd, vram, vsz);
    plane.upload_cram(ctx, cmd, cram, csz);
    barrier(cmd, target.image(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue cc{}; cc.float32[0] = bg[0]; cc.float32[1] = bg[1]; cc.float32[2] = bg[2]; cc.float32[3] = 1.f;
    VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cmd, target.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &range);
    barrier(cmd, target.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    plane.draw_cells(ctx, cmd, cells, count, target.extent().width, target.extent().height, 0);
    barrier(cmd, target.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy r{}; r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    r.imageExtent = { target.extent().width, target.extent().height, 1 };
    vkCmdCopyImageToBuffer(cmd, target.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buf, 1, &r);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    if (vkQueueSubmit(ctx.graphics_queue(), 1, &si, fence) != VK_SUCCESS) return nullptr;
    vkWaitForFences(ctx.device(), 1, &fence, VK_TRUE, UINT64_MAX);
    vkResetFences(ctx.device(), 1, &fence);
    return static_cast<const uint8_t*>(rb.map);
}

// ---- VRAM/CRAM sintéticas ----------------------------------------------------
// Tile `t` en la vista de bus (off^1), nibble alto = píxel par — el inverso
// exacto del desempaquetado de VkIndexedPlane::upload_vram.
static void pack_tile(std::vector<uint8_t>& vram, uint32_t t, const uint8_t idx[64]) {
    for (uint32_t row = 0; row < 8; ++row)
        for (uint32_t bi = 0; bi < 4; ++bi) {
            const uint8_t even = idx[row * 8 + bi * 2] & 0xF, odd = idx[row * 8 + bi * 2 + 1] & 0xF;
            vram[(t * 32 + row * 4 + bi) ^ 1u] = (uint8_t)((even << 4) | odd);
        }
}
static void tile_from(const char* const* rows, uint8_t out[64]) {
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            out[y * 8 + x] = rows[y][x] == '.' ? 0 : (uint8_t)(rows[y][x] - '0');
}

struct Case {
    const char* name;
    const char* rows[8];
    bool hf = false, vf = false;
    // Vecinos opcionales (mismo tile repetido alrededor si `ring` es true).
    const char* const* ring_rows = nullptr;   // nullptr = sin vecinos (clamp)
    bool ring_hf = false;
    // Qué rasgo tiene que mostrar la GPU (no-vacuidad).
    enum Expect { Blend, Alpha, Thin, DiffersFromNoRing } expect;
    float k = 1.0f;   // #503: intensidad (255 = v6)
};

int main() {
    constexpr int S = 6, W = 8 * S;
    const std::string root = AYTHER_SOURCE_DIR;

    if (!SDL_Init(SDL_INIT_VIDEO)) { std::fprintf(stderr, "[FAIL] SDL_Init: %s\n", SDL_GetError()); return 1; }
    SDL_Window* win = SDL_CreateWindow("enhance_shader_smoke", 64, 64, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (!win) { std::fprintf(stderr, "[FAIL] SDL_CreateWindow\n"); return 1; }
    VulkanTestContext ctx;
    if (!ctx.init(win)) { std::fprintf(stderr, "[FAIL] VulkanTestContext::init\n"); return 1; }
    std::printf("GPU: %s\n", ctx.gpu_name().c_str());

    VkRenderTarget target;
    if (!target.init(ctx, W, W)) { std::fprintf(stderr, "[FAIL] target\n"); return 1; }
    VkIndexedPlane plane;
    const std::string sh = root + "/shaders/";
    if (!plane.init(ctx, target, (sh + "indexed_plane.vert.spv").c_str(),
                    (sh + "indexed_plane.frag.spv").c_str())) {
        std::fprintf(stderr, "[FAIL] VkIndexedPlane::init\n"); return 1;
    }
    VkCommandPool pool = VK_NULL_HANDLE; VkCommandBuffer cmd = VK_NULL_HANDLE; VkFence fence = VK_NULL_HANDLE;
    {
        VkCommandPoolCreateInfo pi{}; pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = ctx.graphics_family();
        vkCreateCommandPool(ctx.device(), &pi, nullptr, &pool);
        VkCommandBufferAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(ctx.device(), &ai, &cmd);
        VkFenceCreateInfo fi{}; fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        vkCreateFence(ctx.device(), &fi, nullptr, &fence);
    }
    SmokeReadback rb;
    if (!rb.init(ctx, (VkDeviceSize)W * W * 4)) { std::fprintf(stderr, "[FAIL] readback\n"); return 1; }

    // Paleta (línea 0): índices 1..3 saturados, bien separados; 0 no se dibuja.
    std::vector<uint8_t> cram(0x80, 0);
    const uint16_t packed[16] = { 0, 7, 7 << 3, 7 << 6, 7 | (7 << 3), 7 | (7 << 6),
                                  (7 << 3) | (7 << 6), 7 | (7 << 3) | (7 << 6),
                                  3, 3 << 3, 3 << 6, 3 | (3 << 3), 3 | (3 << 6),
                                  (3 << 3) | (3 << 6), 5, 5 << 3 };
    uint8_t pal[16][3];
    for (int i = 0; i < 16; ++i) {
        cram[i * 2] = (uint8_t)(packed[i] & 0xFF); cram[i * 2 + 1] = (uint8_t)(packed[i] >> 8);
        const uint32_t c = VkIndexedPlane::genesis_color_rgba(packed[i]);   // 0xAABBGGRR
        pal[i][0] = (uint8_t)(c & 0xFF); pal[i][1] = (uint8_t)((c >> 8) & 0xFF); pal[i][2] = (uint8_t)((c >> 16) & 0xFF);
    }
    // Fondo: gris que NO es color de paleta (para leer la silueta por mezcla).
    const uint8_t bg8[3] = { 40, 40, 40 };
    const float   bgf[3] = { 40 / 255.f, 40 / 255.f, 40 / 255.f };

    // ---- casos ---------------------------------------------------------------
    static const char* kDiag45[8]  = { "11111111", "21111111", "22111111", "22211111",
                                       "22221111", "22222111", "22222211", "22222221" };
    static const char* kSlope21[8] = { "11111111", "11111111", "22111111", "22111111",
                                       "22221111", "22221111", "22222211", "22222211" };
    static const char* kSlope31[8] = { "11111111", "11111111", "11111111", "22211111",
                                       "22211111", "22211111", "22222211", "22222211" };
    static const char* kThin[8]    = { "........", "........", "........", "11111111",
                                       "........", "........", "........", "........" };
    static const char* kSil[8]     = { "11111111", ".1111111", "..111111", "...11111",
                                       "....1111", ".....111", "......11", ".......1" };
    static const char* kEdgeR[8]   = { "22221111", "22221111", "22221111", "22221111",
                                       "22222111", "22222111", "22222211", "22222211" };
    static const char* kRing2[8]   = { "22222222", "22222222", "22222222", "22222222",
                                       "22222222", "22222222", "22222222", "22222222" };
    const Case cases[] = {
        { "diagonal 45° opaca (lv1)",              { kDiag45[0], kDiag45[1], kDiag45[2], kDiag45[3], kDiag45[4], kDiag45[5], kDiag45[6], kDiag45[7] }, false, false, nullptr, false, Case::Blend },
        { "pendiente 2:1 (lv2)",                    { kSlope21[0], kSlope21[1], kSlope21[2], kSlope21[3], kSlope21[4], kSlope21[5], kSlope21[6], kSlope21[7] }, false, false, nullptr, false, Case::Blend },
        { "pendiente 3:1 (lv3)",                    { kSlope31[0], kSlope31[1], kSlope31[2], kSlope31[3], kSlope31[4], kSlope31[5], kSlope31[6], kSlope31[7] }, false, false, nullptr, false, Case::Blend },
        { "línea de 1 px (anti-muesca #500)",       { kThin[0], kThin[1], kThin[2], kThin[3], kThin[4], kThin[5], kThin[6], kThin[7] }, false, false, nullptr, false, Case::Thin },
        { "silueta opaco/transparente (alpha)",     { kSil[0], kSil[1], kSil[2], kSil[3], kSil[4], kSil[5], kSil[6], kSil[7] }, false, false, nullptr, false, Case::Alpha },
        { "diagonal 45° con hflip del tile",        { kDiag45[0], kDiag45[1], kDiag45[2], kDiag45[3], kDiag45[4], kDiag45[5], kDiag45[6], kDiag45[7] }, true, false, nullptr, false, Case::Blend },
        { "diagonal 45° con vflip del tile",        { kDiag45[0], kDiag45[1], kDiag45[2], kDiag45[3], kDiag45[4], kDiag45[5], kDiag45[6], kDiag45[7] }, false, true, nullptr, false, Case::Blend },
        { "borde que sigue en el anillo de vecinos", { kEdgeR[0], kEdgeR[1], kEdgeR[2], kEdgeR[3], kEdgeR[4], kEdgeR[5], kEdgeR[6], kEdgeR[7] }, false, false, kRing2, false, Case::DiffersFromNoRing },
        { "anillo de vecinos con hflip",            { kEdgeR[0], kEdgeR[1], kEdgeR[2], kEdgeR[3], kEdgeR[4], kEdgeR[5], kEdgeR[6], kEdgeR[7] }, false, false, kRing2, true, Case::DiffersFromNoRing },
        { "diagonal 45° con k = 0,5 (#503)",        { kDiag45[0], kDiag45[1], kDiag45[2], kDiag45[3], kDiag45[4], kDiag45[5], kDiag45[6], kDiag45[7] }, false, false, nullptr, false, Case::Blend, 0.5f },
        { "silueta con k = 0 (#503: borde al 50 %)", { kSil[0], kSil[1], kSil[2], kSil[3], kSil[4], kSil[5], kSil[6], kSil[7] }, false, false, nullptr, false, Case::Alpha, 0.0f },
    };

    std::printf("=== enhance_shader_smoke (#504) ===  canvas %ux%u (x%d)\n\n", W, W, S);
    constexpr uint32_t kCenter = 1, kRing = 2;   // patrones en VRAM
    std::vector<uint8_t> vram(0x10000, 0);
    std::vector<uint8_t> prev_no_ring;           // para DiffersFromNoRing

    for (const Case& cs : cases) {
        std::printf("-- %s\n", cs.name);
        uint8_t center[64], ring[64] = {};
        tile_from(cs.rows, center);
        std::fill(vram.begin(), vram.end(), 0);
        pack_tile(vram, kCenter, center);
        if (cs.ring_rows) { tile_from(cs.ring_rows, ring); pack_tile(vram, kRing, ring); }

        // Quad: solo el tile central, a ×6, con el anillo en los push constants.
        VkIndexedPlane::CellQuad q{};
        q.x = 0; q.y = 0; q.w = (float)W; q.h = (float)W;
        q.pattern = (uint16_t)kCenter; q.pal = 0;
        q.flips = (uint8_t)((cs.hf ? 1 : 0) | (cs.vf ? 2 : 0));
        q.enhance = 1;
        q.enhance_k = (uint8_t)std::lround(cs.k * 255.0f);   // #503
        if (cs.ring_rows) {
            const uint32_t nb = VkIndexedPlane::CellQuad::neighbor((uint16_t)kRing, cs.ring_hf ? 1 : 0);
            q.nb0 = nb | (nb << 14); q.nb1 = nb | (nb << 14);
            q.nb2 = nb | (nb << 14); q.nb3 = nb | (nb << 14);
        }
        // Primera pasada del caso SIN anillo (para la no-vacuidad del vecino).
        auto run = [&](bool with_ring) -> std::vector<uint8_t> {
            VkIndexedPlane::CellQuad qq = q;
            if (!with_ring) { qq.nb0 = qq.nb1 = qq.nb2 = qq.nb3 = 0; }
            const uint8_t* got = render_and_read(ctx, target, cmd, fence, rb, plane, &qq, 1,
                                                 vram.data(), vram.size(), cram.data(), cram.size(), bgf);
            return got ? std::vector<uint8_t>(got, got + (size_t)W * W * 4) : std::vector<uint8_t>();
        };
        if (cs.expect == Case::DiffersFromNoRing) prev_no_ring = run(false);
        const std::vector<uint8_t> got = run(true);
        if (got.empty()) { check(false, "render"); continue; }

        // Referencia.
        xbr_ref::Tile T; T.idx = center; T.hf = cs.hf; T.vf = cs.vf;
        if (cs.ring_rows) {
            xbr_ref::Neighbor n; n.valid = true; n.idx = ring; n.hf = cs.ring_hf;
            T.left = T.right = T.up = T.down = T.ul = T.ur = T.dl = T.dr = n;
        }
        int written_mismatch = 0, color_mismatch = 0, max_err = 0;
        int gpu_blend = 0, gpu_alpha = 0, ref_blend = 0, ref_alpha = 0, gpu_drawn = 0;
        int worst_x = -1, worst_y = -1;
        for (int y = 0; y < W; ++y)
            for (int x = 0; x < W; ++x) {
                // El vertex shader espeja v_local con los flips: el píxel de
                // pantalla (x, y) es el texel-destino (W-1-x) / (W-1-y).
                const xbr_ref::Out o = xbr_ref::shade(T, cs.hf ? W - 1 - x : x,
                                                      cs.vf ? W - 1 - y : y, S,
                                                      std::lround(cs.k * 255.0f) / 255.0f);
                uint8_t rgba[4]; xbr_ref::resolve(o, pal, rgba);
                // Esperado en el framebuffer: el pase blendea SRC_ALPHA sobre el fondo.
                uint8_t exp[3];
                for (int c = 0; c < 3; ++c) {
                    const float a = o.discard ? 0.f : o.alpha;
                    exp[c] = (uint8_t)std::lround(bg8[c] * (1.f - a) + rgba[c] * a);
                }
                const uint8_t* g = &got[((size_t)y * W + x) * 4];   // BGRA
                const int gr = g[2], gg = g[1], gb = g[0];
                const bool gpu_is_bg = (std::abs(gr - bg8[0]) <= 1 && std::abs(gg - bg8[1]) <= 1 && std::abs(gb - bg8[2]) <= 1);
                const bool ref_is_bg = o.discard || o.alpha <= 0.002f;
                if (!gpu_is_bg) ++gpu_drawn;
                if (gpu_is_bg != ref_is_bg && !(o.alpha > 0.002f && o.alpha < 0.03f)) ++written_mismatch;
                const int err = std::max({ std::abs(gr - exp[0]), std::abs(gg - exp[1]), std::abs(gb - exp[2]) });
                if (err > 2) { ++color_mismatch; }
                if (err > max_err) { max_err = err; worst_x = x; worst_y = y; }
                if (!o.discard && o.blend && o.cov > 0.02f && o.cov < 0.98f) ++ref_blend;
                if (!o.discard && o.alpha > 0.02f && o.alpha < 0.98f) ++ref_alpha;
                // GPU: «mezcla» = color que no es ninguno de la paleta ni el fondo.
                bool is_pal = gpu_is_bg;
                for (int i = 1; i < 16 && !is_pal; ++i)
                    if (std::abs(gr - pal[i][0]) <= 1 && std::abs(gg - pal[i][1]) <= 1 && std::abs(gb - pal[i][2]) <= 1) is_pal = true;
                if (!is_pal) { if (ref_is_bg || o.alpha < 0.98f) ++gpu_alpha; else ++gpu_blend; }
            }
        char buf[160];
        std::snprintf(buf, sizeof(buf), "escritos/descartados coinciden con la referencia (%d desacuerdos)", written_mismatch);
        check(written_mismatch == 0, buf);
        std::snprintf(buf, sizeof(buf), "color dentro de 2 LSB (%d fuera, peor %d en %d,%d)", color_mismatch, max_err, worst_x, worst_y);
        check(color_mismatch == 0, buf);
        check(gpu_drawn > 0, "no-vacuidad: la GPU dibujó algo");
        switch (cs.expect) {
        case Case::Blend:
            std::snprintf(buf, sizeof(buf), "no-vacuidad: mezcla parcial en GPU (%d px) y en la referencia (%d px)", gpu_blend, ref_blend);
            check(gpu_blend > 0 && ref_blend > 0, buf);
            break;
        case Case::Alpha:
            std::snprintf(buf, sizeof(buf), "no-vacuidad: silueta con alpha parcial en GPU (%d px) y referencia (%d px)", gpu_alpha, ref_alpha);
            check(gpu_alpha > 0 && ref_alpha > 0, buf);
            break;
        case Case::Thin:
            std::snprintf(buf, sizeof(buf), "línea íntegra: %d px dibujados (esperado %d), 0 mezclas (%d)", gpu_drawn, 8 * S * S, gpu_blend + gpu_alpha);
            check(gpu_drawn == 8 * S * S && gpu_blend + gpu_alpha == 0, buf);
            break;
        case Case::DiffersFromNoRing: {
            int diff = 0;
            for (size_t i = 0; i < got.size(); i += 4)
                if (std::abs((int)got[i] - (int)prev_no_ring[i]) > 2 ||
                    std::abs((int)got[i + 1] - (int)prev_no_ring[i + 1]) > 2 ||
                    std::abs((int)got[i + 2] - (int)prev_no_ring[i + 2]) > 2) ++diff;
            std::snprintf(buf, sizeof(buf), "no-vacuidad: el anillo de vecinos cambia el resultado (%d px distintos del clamp)", diff);
            check(diff > 0, buf);
            break;
        }
        }
    }

    std::printf("\nLo que este oráculo NO defiende: el valor exacto de fwidth() entre GPUs\n"
                "(tolerancia 2 LSB) y los vecinos por METASPRITE (column-major) — eso es\n"
                "del renderer (ayther_renderer.cpp emit_cell), no del shader.\n");
    std::printf("%d checks, %d fallos\n", g_checks, g_fails);

    vkDeviceWaitIdle(ctx.device());
    rb.shutdown(ctx);
    vkDestroyFence(ctx.device(), fence, nullptr);
    vkDestroyCommandPool(ctx.device(), pool, nullptr);
    plane.shutdown(ctx);
    target.shutdown(ctx);
    ctx.shutdown();
    SDL_DestroyWindow(win);
    SDL_Quit();
    return g_fails ? 1 : 0;
}
