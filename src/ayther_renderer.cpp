// ---------------------------------------------------------------------------
// AytherRenderer — the motor's visual layer (R3). See ayther_renderer.h.
//
// R3.1: renders the emulator frame + resolved HD tiles into the offscreen image
// (blit-based, the same pipeline that used to target the swapchain in the player
// main loop). Leaves the offscreen in its public shader-read handoff layout for
// the frontend to sample or transition temporarily for presentation.
// ---------------------------------------------------------------------------
#include "ayther_renderer.h"
#include "log.h"
#include "ayther_env.h"

#include "ayther_session.h"               // FrameView
#include "ayther_layers.h"                // R-4 (): stack de capas
#include <ayther/engine/vulkan_interop.hpp>
#include "ayther_core_ffi.h"              // AytherTileSub, AyArchive

#include <vk_mem_alloc.h>                 // buffer de readback (export MP4)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>      // getenv (tope de uploads, )
#include <unordered_map>
#include <vector>

namespace ayther {
namespace {

// One image-layout transition.
void image_barrier(VkCommandBuffer cmd, VkImage image,
                   VkImageLayout oldL, VkImageLayout newL,
                   VkAccessFlags srcA, VkAccessFlags dstA,
                   VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
    VkImageMemoryBarrier b{};
    b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout           = oldL;
    b.newLayout           = newL;
    b.srcAccessMask       = srcA;
    b.dstAccessMask       = dstA;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image               = image;
    b.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cmd, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// Blit a source texture (in SHADER_READ_ONLY) to `count` regions of `dst`
// (which must be in TRANSFER_DST). The source is transitioned SRC→back once
// around all regions — repeated tiles cost O(1) transitions, not O(count).
void blit_tex(VkCommandBuffer cmd, VkTexture& src, VkImage dst,
              const VkImageBlit* regions, uint32_t count, VkFilter filter) {
    if (count == 0) return;
    image_barrier(cmd, src.image(),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    for (uint32_t i = 0; i < count; ++i)
        vkCmdBlitImage(cmd, src.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &regions[i], filter);

    image_barrier(cmd, src.image(),
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

// Full source image → a destination rectangle.
VkImageBlit full_to_rect(uint32_t srcW, uint32_t srcH,
                         int32_t dx, int32_t dy, int32_t dw, int32_t dh) {
    VkImageBlit r{};
    r.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    r.srcOffsets[0]  = { 0, 0, 0 };
    r.srcOffsets[1]  = { static_cast<int32_t>(srcW), static_cast<int32_t>(srcH), 1 };
    r.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    r.dstOffsets[0]  = { dx, dy, 0 };
    r.dstOffsets[1]  = { dx + dw, dy + dh, 1 };
    return r;
}

}  // namespace

struct AytherRenderer::FrameScratch {
    struct TileItem {
        VkTexture* tex = nullptr;
        VkImageBlit region{};
    };

    std::vector<VkIndexedPlane::CellQuad> scene_quads;
    std::vector<uint8_t> plane_tile_tint;
    std::vector<uint8_t> plane_tile_alpha;
    std::vector<uint8_t> plane_focused;
    std::vector<uint8_t> sub_has_anchor;
    std::vector<uint8_t> plane_hi_anchor;
    std::vector<uint8_t> plane_hi_drawn;
    std::vector<TileItem> tile_items;
    std::vector<VkImageBlit> tile_group;
    std::vector<AytherSpriteSub> overlay_strip;
    std::vector<uint8_t> overlay_alphas;
    std::vector<uint8_t> overlay_tint;
};

AytherRenderer::AytherRenderer() : scratch_(std::make_unique<FrameScratch>()) {}
AytherRenderer::~AytherRenderer() {
    if (context_.is_valid()) shutdown(context_);
}

namespace {

ayther::engine::RenderImageView render_image_view(
    const VkRenderTarget& target,
    const ayther::engine::VulkanContextView& context) noexcept {
    if (!target.is_ready() || !context.is_valid()) {
        return {};
    }

    return {
        .image = target.image(),
        .image_view = target.view(),
        .sampler = target.sampler(),
        .format = target.format(),
        .extent = target.extent(),
        .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .ready_stage_mask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        .ready_access_mask = VK_ACCESS_SHADER_READ_BIT,
        .queue_family_index = context.graphics_family(),
    };
}

}  // namespace

ayther::engine::RenderImageView AytherRenderer::render_image() const noexcept {
    return render_image_view(target_, context_);
}

ayther::engine::RenderImageView
AytherRenderer::compare_render_image() const noexcept {
    return render_image_view(compare_, context_);
}

bool AytherRenderer::init(const ayther::engine::VulkanContextView& ctx, uint32_t canvas_w,
                          uint32_t canvas_h, const char* shader_dir,
                          const RuntimeOptions& options) {
    options_ = options;
    context_ = ctx;
    if (!context_.is_valid()) {
        return false;
    }
    if (!emu_tex_.init(context_, kEmuW, kEmuH)) {
        ayther::log::write(ayther::log::Severity::Error,
            "renderer", "emu_texture_init_failed",
            "emu texture init failed");
        return false;
    }
    if (!target_.init(context_, canvas_w, canvas_h)) {
        ayther::log::write(ayther::log::Severity::Error,
            "renderer", "offscreen_target_init_failed",
            "offscreen target init failed");
        return false;
    }

    // HD sprite overlay — renders into the offscreen target. Optional: if the
    // SPIR-V shaders are missing, sprites are skipped (emu+tiles still render).
    const std::string dir = shader_dir ? shader_dir : "";
    sprite_ok_ = sprite_.init(context_, target_.format(), canvas_w, canvas_h, target_.view(),
                              (dir + "sprite.vert.spv").c_str(),
                              (dir + "sprite.frag.spv").c_str());
    if (!sprite_ok_)
        ayther::log::write(ayther::log::Severity::Warning,
            "renderer", "sprite_overlay_disabled_shaders",
            "sprite overlay disabled (no shaders)");

    // R-5 (): pipeline indexado del compose sin blit. Opcional como el de
    // sprites: sin shaders, el camino de escena cae al blit del emulador.
    indexed_ok_ = indexed_.init(context_, target_,
                                (dir + "indexed_plane.vert.spv").c_str(),
                                (dir + "indexed_plane.frag.spv").c_str());
    if (!indexed_ok_)
        ayther::log::write(ayther::log::Severity::Warning,
            "renderer", "scene_compose_disabled_shaders",
            "scene compose disabled (no shaders)");
    return true;
}

bool AytherRenderer::resize(const ayther::engine::VulkanContextView& ctx, uint32_t canvas_w, uint32_t canvas_h) {
    if (!target_.resize(ctx, canvas_w, canvas_h)) return false;
    if (sprite_ok_)   // the offscreen view changed — rebuild the sprite framebuffer
        sprite_.rebuild(ctx, canvas_w, canvas_h, target_.view());
    if (indexed_ok_)  // ídem para el framebuffer del compose indexado (R-5)
        indexed_ok_ = indexed_.rebuild(ctx, target_);
    return true;
}

void AytherRenderer::evict_pack_textures(const ayther::engine::VulkanContextView& ctx) {
    tile_cache_.shutdown(ctx);
    if (sprite_ok_) sprite_.clear_textures(ctx);
}

void AytherRenderer::evict_sprite_texture(const ayther::engine::VulkanContextView& ctx, const std::string& path) {
    if (sprite_ok_) sprite_.evict(ctx, path);
}

void AytherRenderer::poll_disk_sprite_textures(const ayther::engine::VulkanContextView& ctx) {
    // Hot-reload de autoría: assets de DISCO reescritos/aparecidos → evict
    // (la recarga la dispara el próximo draw). Ver VkSprite::poll_disk.
    if (sprite_ok_) sprite_.poll_disk(ctx);
}

void AytherRenderer::prewarm_sprite(const std::string& path, uint8_t flip) {
    //  fase 2: decode asincrono anticipado de un asset SUELTO de disco —
    // al alimentar poses, no en su primera aparicion (pico > frame budget →
    // crackle de audio). El upload lo hace pump_uploads en el proximo draw.
    if (sprite_ok_) sprite_.prewarm(path, nullptr, flip);
}

void AytherRenderer::prewarm_sprite_mask(const std::string& path, uint8_t flip) {
    // Vestuario: la máscara paga el mismo pico de decode que el asset.
    if (sprite_ok_) sprite_.prewarm(path, nullptr, flip, /*mask=*/true);
}

void AytherRenderer::shutdown(const ayther::engine::VulkanContextView& ctx) {
    const auto& active_context = context_.is_valid() ? context_ : ctx;
    readback_shutdown(active_context);
    indexed_.shutdown(active_context);
    sprite_.shutdown(active_context);
    tile_cache_.shutdown(active_context);
    emu_tex_.shutdown(active_context);
    compare_.shutdown(active_context);   //  (no-op si el A/B nunca se abrió)
    target_.shutdown(active_context);
    context_ = {};
}

// R-8 (): estado de carga del asset del SUB de un elemento — la clave
// (path + flip) replica EXACTAMENTE la que usan las lanes al dibujar, así el
// checker y el reporte de cobertura miran la misma entrada del cache.
VkSprite::TexState AytherRenderer::sub_texture_state(const FrameView& fv,
                                                     const SceneElement& e) const {
    const char* path = nullptr;
    uint8_t     flip = 0;
    if (e.sub >= 0) {
        if (e.sub_kind == 1 && (uint32_t)e.sub < fv.sprite_sub_count) {
            path = fv.sprite_subs[e.sub].asset_path;
            flip = fv.sprite_sub_flips ? (uint8_t)(fv.sprite_sub_flips[e.sub] & 3) : 0;
        } else if (e.sub_kind == 2 && (uint32_t)e.sub < fv.plane_tile_sub_count) {
            path = fv.plane_tile_subs[e.sub].asset_path;
            flip = fv.plane_tile_flips ? (uint8_t)(fv.plane_tile_flips[e.sub] & 3) : 0;
        }
    }
    if (!path || !path[0]) return VkSprite::TexState::NotRequested;
    return sprite_.texture_state(path, flip);
}

void AytherRenderer::render(const ayther::engine::VulkanContextView& ctx, VkCommandBuffer cmd,
                            const FrameView& fv, AyArchive* pack, bool hd_on,
                            const AytherLayerStack* layers, uint8_t vdp_mask) {
    if (!target_.is_ready()) return;

    // R-4 (): sin stack del caller, el por defecto — misma lista y mismo
    // orden que las lanes cableadas históricas (los smokes no cambian).
    static const AytherLayerStack kDefaultLayers;
    const AytherLayerStack& stack = layers ? *layers : kDefaultLayers;

    // 1. Upload the emulator software framebuffer (skip on a duplicate frame —
    //    emu_tex_ keeps the previous content, still in SHADER_READ_ONLY).
    if (fv.fb_pixels) {
        emu_tex_.upload(ctx, cmd, fv.fb_pixels, fv.fb_width, fv.fb_height,
                        fv.fb_pitch, static_cast<TexPixelFormat::Value>(fv.fb_format));
        // El fb del core cambia de modo (256/320 x 192/224/240) y emu_tex_ se
        // crea al maximo: blitear las dims del FRAME, no las de la textura,
        // o el canvas arrastra una franja muerta abajo/derecha.
        emu_w_ = fv.fb_width  ? fv.fb_width  : emu_w_;
        emu_h_ = fv.fb_height ? fv.fb_height : emu_h_;
    }

    const VkExtent2D canvas = target_.extent();

    // 2. Offscreen UNDEFINED -> TRANSFER_DST (every blit writes here).
    image_barrier(cmd, target_.image(),
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        0, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    //  fase 2: subir al GPU los decodes de textura terminados (worker
    // asincrono de VkSprite) — 1 vez por frame, ANTES de los pases que las
    // consumen, y aunque este frame no haya subs (el prewarm progresa igual).
    //
    // EL TOPE Y SU HISTORIA. Se fijó en 1 creyendo que un upload costaba ~3,5 ms
    // y que 2 por frame drenarían el backlog de audio.  mostró que el 92%
    // de esos 3,5 ms era un fprintf contra la consola: el costo real es 0,44 ms
    // de media (0,077 fijo + 0,375 de memcpy), o sea diez veces menos. Con eso
    // la justificación original ya no se sostiene.
    //
    // MEDIDO (), reproduciendo Demo Amazona a ritmo real, 128 uploads y
    // ~403 MB en cada corrida:
    //
    //     tope   media    max     starved   frames >20 ms
    //       1    0,63 ms  2,64       0           11
    //       2    0,51 ms  4,06       0           10
    //       4    0,36 ms  1,52       0           52
    //
    // El AUDIO aguanta los tres (starved 0), así que el límite no es el mixer:
    // es la FLUIDEZ. Con 4 los frames lentos se quintuplican, y el probe sólo
    // muestrea 1 vez cada 500 ms — 52 sobre ~80 ventanas posibles es sostenido,
    // no una ráfaga. Con 2 el costo no se distingue del de 1 y los assets
    // drenan al doble de velocidad: menos pop-in por el mismo precio.
    //
    // De ahí el default 2. El env var se queda como escape y para repetir la
    // medición sin recompilar.
    //
    // Y NO se le aplica al VIDEO (): una textura persistente que se re-sube
    // cada frame no crea imagen, ni mips, ni descriptores — no pasa por acá.
    if (sprite_ok_) sprite_.pump_uploads(ctx, cmd, options_.upload_budget());
    // : liberar el staging de tiles ya garantizados por fence (1×/frame).
    tile_cache_.pump(ctx);

    // R-4 (): las lanes se despachan desde el STACK DE CAPAS, en su orden.
    // Cada cuerpo es la lane histórica intacta (comentarios incluidos); lo que
    // antes era el orden textual de los ifs ahora es la posición en la lista.
    // Las 4 capas VDP del prefijo disparan el blit del emulador UNA vez (su
    // visibilidad individual viaja por la máscara de sesión, no por acá); una
    // capa oculta se saltea; una Custom no dibuja nada todavía (R-7).
    bool emu_blitted = false;

    // R-5: estado compartido de los DOS pases de escena (base pri-0 en el
    // prefijo VDP; primer plano pri-1 en VdpFrente, sobre las lanes HD).
    const bool scene_ready = fv.scene && fv.scene_count && fv.scene_vram &&
                             fv.scene_cram && !fv.scene_dirty &&
                             indexed_ok_ && emu_w_ && emu_h_;
    bool scene_composed = false;   // el pase base corrió → VdpFrente dibuja
    bool scene_vis[4] = { true, true, true, true };   // B, A, W, Sprites
    bool pano_lane_vis = true;     // ojo de la lane Panorámica (gate del inline)
    bool spr_lane_visible = true;  // ojo de Sprites HD (gate de los pri-1 diferidos)
    for (const AytherLayer& vl : stack.layers()) {
        switch (vl.kind) {
            case AytherLayerKind::VdpPlaneB:  scene_vis[0] = vl.visible; break;
            case AytherLayerKind::VdpPlaneA:  scene_vis[1] = vl.visible; break;
            case AytherLayerKind::VdpWindow:  scene_vis[2] = vl.visible; break;
            case AytherLayerKind::VdpSprites: scene_vis[3] = vl.visible; break;
            case AytherLayerKind::Panorama:   pano_lane_vis = vl.visible; break;
            case AytherLayerKind::SpritesHd:  spr_lane_visible = vl.visible; break;
            default: break;
        }
    }
    // Ojos de workspace (ex canal 0x102): AND con el stack, por elemento.
    scene_vis[0] = scene_vis[0] && (vdp_mask & 0x02);   // B
    scene_vis[1] = scene_vis[1] && (vdp_mask & 0x01);   // A
    scene_vis[2] = scene_vis[2] && (vdp_mask & 0x04);   // Window
    scene_vis[3] = scene_vis[3] && (vdp_mask & 0x08);   // Sprites
    //  fase 0: el ancho LÓGICO manda sobre el nativo. Con `wide_w` el
    // frame del emulador se centra y a los lados queda el área extendida; sin
    // él, `logical_w == emu_w_` y todo sale exactamente como siempre.
    const uint32_t logical_w = fv.wide_w > emu_w_ ? fv.wide_w : emu_w_;
    const float scene_sx = logical_w ? (float)canvas.width / (float)logical_w : 1.f;
    // Desplazamiento de CENTRADO, en píxeles del canvas. Es lo único que hay
    // que sumar a cada x: el resto del pipeline ya trabaja en espacio emu.
    const float wide_dx = 0.5f * (float)(logical_w - emu_w_) * scene_sx;
    // El mismo corrimiento en espacio EMU, para los quads de sprite: sus subs
    // vienen en coordenadas nativas y el pase los escala por su cuenta.
    const float wide_dx_emu = 0.5f * (float)(logical_w - emu_w_);
    sprite_.set_wide_offset(wide_dx_emu);
    const float scene_sy = emu_h_ ? (float)canvas.height / (float)emu_h_ : 1.f;
    const int32_t scene_scissor =
        fv.scene_left_blank ? (int32_t)(8.f * scene_sx + 0.5f) : 0;
    auto& scene_quads = scratch_->scene_quads;
    // Subs de sprite ANCLADOS a un elemento de la escena: los despacha el pase
    // de escena, en el z de la cadena. Se marca por la sola EXISTENCIA del ancla
    // en el inventario — no por haber dibujado. Esa distinción es la que hace que
    // ocultar apague de verdad el HD (2026-07-31): si el ancla se saltea (ojo del
    // Inspector, o su capa apagada desde el header del timeline), marcarlo sólo
    // al dibujar dejaba el sub en 0 y la lane 4S lo re-dibujaba como si fuera un
    // sub SIN ancla — la pose con asset seguía viéndose. La lane 4S queda para su
    // propósito real: los subs sin ancla (slot 255 / entity-like).
    // Capa ENFOCADA () en las lanes HD. Las lanes no llevan capa propia —
    // REEMPLAZAN contenido de una — así que el atenuado se decide por la capa a
    // la que sustituyen: Sprites/Modo 3/Anim son de la capa Sprites; Cuadro,
    // Panorámica, tiles de plano y video son del FONDO. Sin esto la capa
    // enfocada sólo se notaría en lo que NO tiene asset, o sea al revés.
    // Los tiles de plano sí se resuelven celda por celda más abajo (el sub sabe
    // su elemento, y el elemento su capa); el Cuadro cubre el 100% de la
    // pantalla y no distingue A de B por construcción.
    // El atenuado son DOS cosas: oscurecer a 0.25 y componer al 75% de opacidad.
    // La opacidad no es decorativa — oscurecer solo deja el HD igual de OPACO, y
    // una capa de fondo atenuada seguía tapando por completo a la enfocada que
    // tiene detrás (el caso normal al autorar el Plano B con un Cuadro delante).
    const float dim_lo   = 0.25f;
    const float dim_op   = 0.75f;
    const bool  focusing = focus_layer_ >= 0;
    const float dim_spr  = focusing && focus_layer_ != 3 ? dim_lo : 1.0f;
    const float dim_bg   = focusing && focus_layer_ >  1 ? dim_lo : 1.0f;
    const float op_spr   = dim_spr < 1.0f ? dim_op : 1.0f;
    const float op_bg    = dim_bg  < 1.0f ? dim_op : 1.0f;
    // Tiles de plano: el sub SÍ sabe su capa (por el elemento que lo reclama),
    // así que A y B se distinguen. Una sola llamada mezcla capas, así que el
    // atenuado va POR SUB — tinte Q2.6 (64 = 1.0, 16 = 0.25) y opacidad 0-255.
    auto& plane_tile_tint = scratch_->plane_tile_tint;
    auto& plane_tile_alpha = scratch_->plane_tile_alpha;
    plane_tile_tint.clear();
    plane_tile_alpha.clear();
    // Tinte E1 de la sesión (quads de SET con referencia autorada — el Objeto
    // sigue los fundidos de paleta): base del vector, sobre TODO el rango
    // [0,count) porque la lane HI y el pase inline también lo consumen.
    if (fv.plane_tile_sub_tint && fv.plane_tile_sub_count > 0)
        plane_tile_tint.assign(fv.plane_tile_sub_tint,
                               fv.plane_tile_sub_tint +
                                   (size_t)fv.plane_tile_sub_count * 3);
    if (focusing && fv.plane_tile_sub_hi > 0) {
        if (plane_tile_tint.empty())
            plane_tile_tint.assign((size_t)fv.plane_tile_sub_count * 3, 64);
        plane_tile_alpha.assign((size_t)fv.plane_tile_sub_hi,
                                (uint8_t)(255 * dim_op));
        auto& plane_focused = scratch_->plane_focused;
        plane_focused.assign(fv.plane_tile_sub_hi, 0);
        if (scene_ready)
            for (uint32_t i = 0; i < fv.scene_count; ++i) {
                const SceneElement& e = fv.scene[i];
                if (e.sub_kind == 2 && e.sub >= 0 &&
                    (uint32_t)e.sub < fv.plane_tile_sub_hi &&
                    (int)e.layer == focus_layer_)
                    plane_focused[e.sub] = 1;
            }
        // El atenuado COMPONE sobre el tinte E1 (0.25× de lo que el sub ya
        // traiga), no lo pisa — un set con fundido y fuera de foco queda
        // fundido Y atenuado. La capa enfocada conserva su E1 intacto.
        const uint32_t dimq = (uint32_t)(64 * dim_lo + 0.5f);   // Q2.6
        for (uint32_t s = 0; s < fv.plane_tile_sub_hi; ++s) {
            if (plane_focused[s]) { plane_tile_alpha[s] = 255; continue; }
            for (int c = 0; c < 3; ++c) {
                uint8_t& v = plane_tile_tint[(size_t)s * 3 + c];
                v = (uint8_t)(((uint32_t)v * dimq) >> 6);
            }
        }
    }

    auto& sub_has_anchor = scratch_->sub_has_anchor;
    sub_has_anchor.assign(fv.sprite_sub_count, 0);
    // Los subs de plano de ALTA prioridad con elemento en la escena se dibujan
    // INLINE en el pase pri-1 (z real de la cadena: un sprite pri-1 HD queda
    // DELANTE — las letras del título de GA sobre el isologotipo); la lane
    // PlaneTilesHi los saltea para no re-dibujarlos encima (mismo patrón que
    // la Panorámica: la lane queda como fallback del híbrido).
    auto& plane_hi_anchor = scratch_->plane_hi_anchor;
    plane_hi_anchor.assign(fv.plane_tile_sub_count, 0);
    // Un quad de SET (Objeto) es el sub de CIENTOS de elementos (el
    // isologotipo: 277 celdas) — se dibuja UNA vez, en el z del primero.
    auto& plane_hi_drawn = scratch_->plane_hi_drawn;
    plane_hi_drawn.assign(fv.plane_tile_sub_count, 0);
    if (scene_ready)
        for (uint32_t i = 0; i < fv.scene_count; ++i) {
            const SceneElement& e = fv.scene[i];
            if (e.layer == 3 && e.sub_kind == 1 && e.sub >= 0 &&
                (uint32_t)e.sub < fv.sprite_sub_count)
                sub_has_anchor[e.sub] = 1;
            if (e.layer < 3 && e.sub_kind == 2 &&
                e.sub >= (int32_t)fv.plane_tile_sub_hi &&
                (uint32_t)e.sub < fv.plane_tile_sub_count)
                plane_hi_anchor[e.sub] = 1;
        }
    // Dibuja un PASE de escena con los subs HD de sprites INLINE en su
    // posición del orden (la del ancla en la cadena): así una occ ajena
    // DELANTE de una pose HD queda delante (el caso de la amazona sobre el
    // dragón, que antes restauraba el compose 4C muerto). R-7: `want_layer`
    // filtra a UNA capa VDP (>=0) — cada capa del stack dibuja su pase en SU
    // posición, y una Custom intercalada queda de verdad entre ellas; -1 =
    // todas (el pase pri-1 de VdpFrente). Convención de layouts: el offscreen
    // entra y sale en TRANSFER_DST; cada flush de quads hace su barrera.
    //  v2: mapa (capa, x, y) → celda de PLANO del inventario, para que el
    // EPX de una celda mejorada vea a sus vecinas de la misma capa (y paleta:
    // índices de otra línea no son comparables). Se arma UNA vez por frame y
    // sólo si hay algo que mejorar — el camino sin enhance no paga nada.
    struct PlaneCellRef { uint16_t pattern; uint8_t flips, palette; };
    std::unordered_map<uint64_t, PlaneCellRef> plane_cells_by_pos;
    bool plane_cells_indexed = false;
    auto plane_key = [](uint8_t layer, int x, int y) -> uint64_t {
        return ((uint64_t)layer << 48) | ((uint64_t)(uint32_t)(x + 0x8000) << 16)
             | (uint64_t)(uint32_t)(y + 0x8000);
    };
    auto index_plane_cells = [&]() {
        if (plane_cells_indexed) return;
        plane_cells_indexed = true;
        for (uint32_t i = 0; i < fv.scene_count; ++i) {
            const SceneElement& c = fv.scene[i];
            if (c.layer > 2 || c.hidden) continue;
            plane_cells_by_pos.emplace(plane_key(c.layer, c.x, c.y),
                                       PlaneCellRef{ c.pattern, c.flips, c.palette });
        }
    };
    auto plane_neighbor = [&](const SceneElement& e, int dx, int dy) -> uint32_t {
        auto it = plane_cells_by_pos.find(plane_key(e.layer, e.x + dx, e.y + dy));
        if (it == plane_cells_by_pos.end() || it->second.palette != e.palette) return 0u;
        return VkIndexedPlane::CellQuad::neighbor(it->second.pattern, it->second.flips);
    };
    // : lo mismo para SPRITES mejorados que se tocan (las letras del logo
    // son 4 sprites): mapa (x, y) → tile de cualquier sprite con fx_enhance,
    // expandido tile a tile con el mismo modelo column-major del emit. Se usa
    // cuando el vecino cae FUERA del propio metasprite; exige misma paleta y
    // misma prioridad (mismo pase de dibujo). Lazy y sólo con enhance.
    struct SpriteTileRef { uint16_t pattern; uint8_t flips, palette, priority; };
    std::unordered_map<uint64_t, SpriteTileRef> sprite_tiles_by_pos;
    bool sprite_tiles_indexed = false;
    auto index_sprite_tiles = [&]() {
        if (sprite_tiles_indexed) return;
        sprite_tiles_indexed = true;
        for (uint32_t i = 0; i < fv.scene_count; ++i) {
            const SceneElement& s = fv.scene[i];
            if (s.layer != 3 || !s.fx_enhance || s.hidden || s.claimed) continue;
            const int wt = s.w / 8, ht = s.h / 8;
            for (int col = 0; col < wt; ++col)
                for (int row = 0; row < ht; ++row) {
                    const int dc = (s.flips & 1) ? wt - 1 - col : col;
                    const int dr = (s.flips & 2) ? ht - 1 - row : row;
                    sprite_tiles_by_pos.emplace(
                        plane_key(3, s.x + dc * 8, s.y + dr * 8),
                        SpriteTileRef{ (uint16_t)((s.pattern + col * ht + row) & 0x7FF),
                                       s.flips, s.palette, s.priority });
                }
        }
    };
    auto sprite_neighbor = [&](const SceneElement& e, int x, int y) -> uint32_t {
        index_sprite_tiles();
        auto it = sprite_tiles_by_pos.find(plane_key(3, x, y));
        if (it == sprite_tiles_by_pos.end() || it->second.palette != e.palette ||
            it->second.priority != e.priority) return 0u;
        return VkIndexedPlane::CellQuad::neighbor(it->second.pattern, it->second.flips);
    };
    auto draw_scene_pass = [&](uint8_t want_pri, int want_layer) {
        scene_quads.clear();
        auto flush_quads = [&]() {
            if (scene_quads.empty()) return;
            image_barrier(cmd, target_.image(),
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            indexed_.draw_cells(ctx, cmd, scene_quads.data(), scene_quads.size(),
                                canvas.width, canvas.height, scene_scissor);
            scene_quads.clear();
        };
        for (uint32_t i = 0; i < fv.scene_count; ++i) {
            const SceneElement& e = fv.scene[i];
            if (e.priority != want_pri) continue;
            if (want_layer >= 0 && (int)e.layer != want_layer) continue;
            if (e.layer > 3 || !scene_vis[e.layer] || e.hidden) continue;
            //  EM-8.3: sprites que entran y salen del área nueva.
            //
            // Con el ensanchado, un sprite que el VDP recorta contra el borde
            // nativo se dibuja ENTERO — se ve entrar y salir en vez de cortarse
            // en el aire, que es lo que la tarea pide y sale gratis: el pase de
            // sprites ya lleva el corrimiento y el canvas es más ancho.
            //
            // Lo que NO sale gratis es lo que aparece de yapa. Estacionar los
            // sprites que no se usan justo afuera de la pantalla es un idiom de
            // Genesis —siguen en la SAT, con su patrón, quietos— y ensanchar
            // los pone A LA VISTA: basura que el juego nunca quiso mostrar,
            // flotando en los laterales.
            //
            // La regla: un sprite entra si TOCA la pantalla nativa. Los que
            // están enteros afuera no se dibujan. No se puede distinguir «está
            // por entrar» de «está estacionado» —los dos son un sprite quieto
            // afuera— y de los dos errores posibles, mostrar basura es visible
            // y permanente, mientras que un sprite que aparece unos píxeles
            // tarde no se nota. Sin ensanchar la condición es vacía: el sprite
            // fuera de pantalla tampoco se veía antes.
            if (e.layer == 3 && logical_w > emu_w_ && emu_w_) {
                const int32_t x0 = e.x, x1 = e.x + (int32_t)e.w;
                if (x1 <= 0 || x0 >= (int32_t)emu_w_) continue;
            }
            // R-8 (): modo UV checker — la vista responde «¿qué falta?».
            //   cat 1: nunca autorado (sin asset) → el par de SU CAPA
            //   cat 2: autorado pero el asset NO cargó (negative-cache) → el
            //          par de ALERTA (rosa+negro), igual en las cuatro capas
            //   cat 0:           tiene asset → original atenuado; el HD dibuja
            //                    normal encima (el contraste salta solo).
            // Los efectos R-6 del elemento se ignoran acá: es una vista de
            // diagnóstico, no una composición.
            int checker_cat = 0;
            if (checker_) {
                if (!e.claimed) checker_cat = 1;
                else if (e.sub >= 0 &&
                         sub_texture_state(fv, e) == VkSprite::TexState::Failed)
                    checker_cat = 2;
            }
            const bool hd_replaces = hd_on && e.claimed && checker_cat == 0;
            if (hd_replaces && !checker_) {
                // El HD reemplaza al elemento. Si este es el ANCLA de un sub de
                // sprite, el sub se dibuja ACÁ (z de la escena, no lane plana).
                if (e.layer == 3 && e.sub >= 0 && e.sub_kind == 1 &&
                    (uint32_t)e.sub < fv.sprite_sub_count && sprite_ok_) {
                    flush_quads();
                    sprite_.set_dim(dim_spr, op_spr);
                    sprite_.draw(ctx, cmd, target_.image(),
                                 fv.sprite_subs + e.sub, 1, pack,
                                 logical_w ? logical_w : kEmuW, emu_h_ ? emu_h_ : kEmuH,
                                 fv.sprite_sub_flips ? fv.sprite_sub_flips + e.sub : nullptr,
                                 fv.sprite_sub_tint  ? fv.sprite_sub_tint + e.sub * 3 : nullptr,
                                 fv.sprite_sub_slot  ? fv.sprite_sub_slot + e.sub : nullptr);
                }
                // Ídem para un tile de plano de ALTA prioridad: su HD va en el
                // z de la cadena, no en la lane PlaneTilesHi — un sprite pri-1
                // (que la escena ordena DESPUÉS) queda delante, como en el
                // hardware (sp1 > A1: las letras del título de GA sobre el
                // isologotipo). La lane los saltea vía plane_hi_anchor.
                if (e.layer < 3 && e.sub_kind == 2 &&
                    e.sub >= (int32_t)fv.plane_tile_sub_hi &&
                    (uint32_t)e.sub < fv.plane_tile_sub_count && sprite_ok_ &&
                    !plane_hi_drawn[e.sub]) {
                    plane_hi_drawn[e.sub] = 1;   // un set = cientos de elementos
                    flush_quads();
                    sprite_.set_dim(1.0f);   // como la lane: atenuado por sub
                    sprite_.draw(ctx, cmd, target_.image(),
                                 fv.plane_tile_subs + e.sub, 1, pack,
                                 logical_w ? logical_w : kEmuW, emu_h_ ? emu_h_ : kEmuH,
                                 fv.plane_tile_flips ? fv.plane_tile_flips + e.sub
                                                     : nullptr,
                                 plane_tile_tint.empty()
                                     ? nullptr
                                     : plane_tile_tint.data() + (size_t)e.sub * 3);
                }
                continue;
            }
            // R-6: efectos del elemento → uniform por quad. La SILUETA se
            // emite ANTES (detrás) como los mismos quads expandidos ~2px en
            // modo flat — resalta el contorno del elemento para la autoría.
            // R-8: en modo checker los efectos se pisan — cat 0 dibuja el
            // original ATENUADO (tinte 0.5×) para que lo sin mapear salte.
            // Capa enfocada: lo que NO se está autorando se atenúa a 0.25x
            // (0x10 en Q2.6) y al 75% de la opacidad que tuviera. Pisa el tinte
            // de R-6, igual que el checker: las tres son vistas de autoría y la
            // última que se pide manda.
            const bool dimmed = focus_layer_ >= 0 && (int)e.layer != focus_layer_;
            const uint32_t fx = checker_
                ? 0xFF202020u
                : dimmed
                ? (0x101010u |
                   ((uint32_t)(e.fx_opacity * 3u / 4u) << 24))
                : (uint32_t)e.fx_tint[0] |
                  ((uint32_t)e.fx_tint[1] << 8) |
                  ((uint32_t)e.fx_tint[2] << 16) |
                  ((uint32_t)e.fx_opacity << 24);
            const float ox = 2.f * scene_sx, oy = 2.f * scene_sy;
            // outline=true emite la SILUETA (expandida ~2px, modo flat);
            // false, el arte. Para multi-tile, TODAS las siluetas del
            // elemento van primero — si se intercalaran, la silueta de un
            // tile pisaría el borde del arte del anterior.
            // R-8: palabra de checker por celda — bit0 on · bit1 par · el
            // offset LOCAL del quad dentro del elemento (px de emu) ancla el
            // patrón al elemento (un tile suelto re-estampa, uno grande fluye).
            auto emit_cell = [&](float x, float y, uint16_t pattern, bool outline,
                                 int lx, int ly, uint32_t nb0 = 0, uint32_t nb1 = 0,
                                 uint32_t nb2 = 0, uint32_t nb3 = 0) {
                VkIndexedPlane::CellQuad q;
                q.pattern = pattern;
                q.pal     = e.palette;
                q.flips   = e.flips;
                //  fase 0: el centrado del ensanchado. Un solo lugar —
                // todas las celdas de escena pasan por acá— y con wide_dx = 0
                // (el caso sin ensanchar) la suma no cambia un píxel.
                x += wide_dx;
                if (outline) {
                    q.x = x - ox;            q.y = y - oy;
                    q.w = 8.f * scene_sx + 2 * ox;
                    q.h = 8.f * scene_sy + 2 * oy;
                    q.flat_rgba = e.fx_outline;
                } else {
                    q.x = x;                 q.y = y;
                    q.w = 8.f * scene_sx;    q.h = 8.f * scene_sy;
                    q.fx = fx;
                    // : «Mejorar por software» — el gate hd_on cubre
                    // gratis el A/B «Original» () y el export sin HD; en
                    // modo checker las vistas de autoría mandan.
                    q.enhance = (hd_on && !checker_ && e.fx_enhance) ? 1 : 0;
                    if (q.enhance) {   // v2/v3: los 8 tiles vecinos
                        q.nb0 = nb0; q.nb1 = nb1; q.nb2 = nb2; q.nb3 = nb3;
                        q.enhance_k = e.fx_enhance_k;   // 
                    }
                    if (checker_cat)
                        q.checker = 1u | (checker_cat == 2 ? 2u : 0u) |
                                    // capa (bits 2-3): cada plano tiene su par
                                    // de colores en el shader.
                                    (((uint32_t)e.layer & 3u) << 2) |
                                    ((uint32_t)(lx & 0xFFF) << 8) |
                                    ((uint32_t)(ly & 0xFFF) << 20);
                }
                scene_quads.push_back(q);
            };
            //  v2: sólo el quad mejorado resuelve vecinos (el resto no
            // paga nada). Se resuelven en espacio de PANTALLA: el shader
            // deshace los flips de cada tile al hacer el fetch.
            const bool want_nb = hd_on && !checker_ && e.fx_enhance;
            auto emit_element = [&](bool outline) {
                if (e.layer != 3) {
                    uint32_t nb0 = 0, nb1 = 0, nb2 = 0, nb3 = 0;
                    if (want_nb && !outline) {
                        index_plane_cells();
                        nb0 = plane_neighbor(e, -8,  0) | (plane_neighbor(e, 8,  0) << 14);
                        nb1 = plane_neighbor(e,  0, -8) | (plane_neighbor(e, 0,  8) << 14);
                        nb2 = plane_neighbor(e, -8, -8) | (plane_neighbor(e, 8, -8) << 14);
                        nb3 = plane_neighbor(e, -8,  8) | (plane_neighbor(e, 8,  8) << 14);
                    }
                    emit_cell(e.x * scene_sx, e.y * scene_sy, e.pattern, outline,
                              0, 0, nb0, nb1, nb2, nb3);
                    return;
                }
                // Sprite: w×h tiles COLUMN-MAJOR desde el tile base; el flip
                // del metasprite espeja el orden de tiles Y cada tile (modelo
                // validado bit a bit por scene_inventory_smoke).
                const int wt = e.w / 8, ht = e.h / 8;
                // Tile que ocupa la celda de PANTALLA (dc, dr) del metasprite;
                // 0 = fuera del sprite. Todos comparten flips y paleta.
                auto tile_at = [&](int dc, int dr) -> uint32_t {
                    if (dc < 0 || dr < 0 || dc >= wt || dr >= ht)   // : otro sprite
                        return sprite_neighbor(e, e.x + dc * 8, e.y + dr * 8);
                    const int col = (e.flips & 1) ? wt - 1 - dc : dc;
                    const int row = (e.flips & 2) ? ht - 1 - dr : dr;
                    return VkIndexedPlane::CellQuad::neighbor(
                        (uint16_t)((e.pattern + col * ht + row) & 0x7FF), e.flips);
                };
                for (int col = 0; col < wt; ++col)
                    for (int row = 0; row < ht; ++row) {
                        const int dc = (e.flips & 1) ? wt - 1 - col : col;
                        const int dr = (e.flips & 2) ? ht - 1 - row : row;
                        uint32_t nb0 = 0, nb1 = 0, nb2 = 0, nb3 = 0;
                        if (want_nb && !outline) {
                            nb0 = tile_at(dc - 1, dr)     | (tile_at(dc + 1, dr)     << 14);
                            nb1 = tile_at(dc, dr - 1)     | (tile_at(dc, dr + 1)     << 14);
                            nb2 = tile_at(dc - 1, dr - 1) | (tile_at(dc + 1, dr - 1) << 14);
                            nb3 = tile_at(dc - 1, dr + 1) | (tile_at(dc + 1, dr + 1) << 14);
                        }
                        emit_cell((e.x + dc * 8) * scene_sx,
                                  (e.y + dr * 8) * scene_sy,
                                  (uint16_t)((e.pattern + col * ht + row) & 0x7FF),
                                  outline, dc * 8, dr * 8, nb0, nb1, nb2, nb3);
                    }
            };
            if (e.fx_outline && !checker_) emit_element(true);
            emit_element(false);
            // R-8 con asset (cat 0): el sub inline del ancla dibuja NORMAL
            // sobre su original atenuado — «tiene asset» se ve, no se cuenta.
            if (hd_replaces && checker_ &&
                e.layer == 3 && e.sub >= 0 && e.sub_kind == 1 &&
                (uint32_t)e.sub < fv.sprite_sub_count && sprite_ok_) {
                flush_quads();
                sprite_.draw(ctx, cmd, target_.image(),
                             fv.sprite_subs + e.sub, 1, pack,
                             logical_w ? logical_w : kEmuW, emu_h_ ? emu_h_ : kEmuH,
                             fv.sprite_sub_flips ? fv.sprite_sub_flips + e.sub : nullptr,
                             fv.sprite_sub_tint  ? fv.sprite_sub_tint + e.sub * 3 : nullptr,
                             fv.sprite_sub_slot  ? fv.sprite_sub_slot + e.sub : nullptr);
            }
        }
        flush_quads();
    };

    // R-7: el INIT del compose (uploads + clear al backdrop) va ANTES del
    // loop de capas — una Custom puede estar incluso DELANTE del plano B, y
    // dibujar antes de un clear diferido la borraría. El blit del fallback
    // sigue disparándose en la primera capa VDP.
    if (scene_ready) {
        indexed_.upload_vram(ctx, cmd, fv.scene_vram, fv.scene_vram_size);
        indexed_.upload_cram(ctx, cmd, fv.scene_cram, fv.scene_cram_size);
        const uint32_t bd = VkIndexedPlane::genesis_color_rgba(fv.scene_backdrop);
        VkClearColorValue cc{};
        cc.float32[0] = (float)((bd >>  0) & 0xFF) / 255.f;
        cc.float32[1] = (float)((bd >>  8) & 0xFF) / 255.f;
        cc.float32[2] = (float)((bd >> 16) & 0xFF) / 255.f;
        cc.float32[3] = 1.f;
        VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdClearColorImage(cmd, target_.image(),
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &range);
        scene_composed = true;
    }

    for (const AytherLayer& L : stack.layers()) {
        switch (L.kind) {
        case AytherLayerKind::VdpPlaneB:
        case AytherLayerKind::VdpPlaneA:
        case AytherLayerKind::VdpWindow:
        case AytherLayerKind::VdpSprites: {
            // R-7: cada capa VDP dibuja SU pase pri-0 en SU posición del
            // stack — una Custom intercalada queda de verdad entre B y A. El
            // pri-1 de todas va en VdpFrente (sobre las lanes HD, R-5). La
            // visibilidad es por elemento (stack ∧ vdp_mask, sin canal).
            if (scene_composed) {
                const int li = L.kind == AytherLayerKind::VdpPlaneB   ? 0
                             : L.kind == AytherLayerKind::VdpPlaneA   ? 1
                             : L.kind == AytherLayerKind::VdpWindow   ? 2 : 3;
                if (scene_vis[li]) draw_scene_pass(0, li);
                // PANORÁMICA en el z de SU plano: la tira reemplaza celdas de
                // ESTA capa, así que sus quads van acá — encima del pase del
                // plano y DEBAJO de todo lo que sigue (A/Window/Sprites/pri-1).
                // En la lane global 4R tapaban lo de adelante (reporte
                // 2026-07-30). Convención: pano plane 0=A · 1=B ↔ li 0=B · 1=A.
                // : el VIDEO de la Cinemática en el z del plano más alto
                // de SU máscara. Antes se dibujaba a pantalla completa y opaco
                // en la lane 4V, así que tapaba los Sprites y el Window VIVOS
                // del juego durante todo el clip. Es la misma falla que tuvo la
                // Panorámica y se corrigió igual: inline en el pase del plano
                // que reemplaza. Convención: máscara bit0=A · bit1=B · bit2=Win
                // ↔ li 0=B · 1=A · 2=Window; el "más alto" es el mayor li, que
                // es el último que se dibuja antes de lo que debe quedar encima.
                //  decisión 3: con `video_front` el z no es el del plano
                // sino el del FRENTE (lane VdpFrente, más abajo). El dato lo
                // decide la sesión contando la prioridad VDP de las celdas
                // vivas — un Cuadro mayoritariamente pri-1 es un primer plano y
                // el video puesto atrás quedaría detrás de lo que viene a tapar.
                const int vid_li = (!fv.video_plane_mask || fv.video_front) ? -1
                                 : (fv.video_plane_mask & 0x04) ? 2
                                 : (fv.video_plane_mask & 0x01) ? 1
                                 : (fv.video_plane_mask & 0x02) ? 0 : -1;
                if (li == vid_li && hd_on && sprite_ok_ && scene_vis[li] &&
                    fv.video_y && fv.video_u && fv.video_v &&
                    fv.video_w && fv.video_h) {
                    const bool vdim = focusing && focus_layer_ != li;
                    sprite_.set_dim(vdim ? dim_lo : 1.0f, vdim ? dim_op : 1.0f);
                    sprite_.draw_video(ctx, cmd, target_.image(),
                                       static_cast<const uint8_t*>(fv.video_y), fv.video_y_stride,
                                       static_cast<const uint8_t*>(fv.video_u), fv.video_u_stride,
                                       static_cast<const uint8_t*>(fv.video_v), fv.video_v_stride,
                                       fv.video_w, fv.video_h, fv.video_seq);
                }
                const int pano_li = fv.panorama_plane == 0 ? 1 : 0;
                if (li == pano_li && li <= 1 && hd_on && sprite_ok_ &&
                    pano_lane_vis && scene_vis[li] && fv.panorama_sub_count > 0) {
                    // Acá la capa es EXACTA: la tira reemplaza celdas de ESTA.
                    const bool pano_dim = focusing && focus_layer_ != li;
                    sprite_.set_dim(pano_dim ? dim_lo : 1.0f,
                                    pano_dim ? dim_op : 1.0f);
                    //  fase 0: la tira se mapea con el ancho LÓGICO — sus
                    // coordenadas ya vienen en ese espacio desde la sesión. Sin
                    // ensanchar, logical_w == emu_w_ y no cambia nada.
                    // La tira ya viene en espacio LÓGICO desde la sesión, así
                    // que no lleva el corrimiento de los quads nativos.
                    sprite_.set_wide_offset(0.0f);
                    sprite_.draw(ctx, cmd, target_.image(),
                                 fv.panorama_subs, fv.panorama_sub_count, pack,
                                 logical_w ? logical_w : kEmuW, emu_h_ ? emu_h_ : kEmuH,
                                 nullptr, fv.panorama_sub_tint);   // fundido E1
                    sprite_.set_wide_offset(wide_dx_emu);
                }
                break;
            }
            // Fallback (híbrido de R-1: frame sucio / sin escena / sin
            // shaders): el blit del emulador, exacto, UNA vez. Las capas
            // Custom intercaladas quedan aproximadas ese frame (transiciones).
            if (emu_blitted) break;
            emu_blitted = true;
            if (emu_tex_.is_ready()) {
                //  fase 0: el blit va CENTRADO en el ancho lógico, no a
                // todo el canvas. Mandarlo al canvas entero no ensancha nada:
                // ESTIRA el frame nativo, y la salida de 398 reescalada a 320
                // vuelve byte-idéntica a la nativa (medido: 0 píxeles
                // distintos) — o sea, cero campo de visión ganado con la
                // imagen deformada de yapa. Los lados quedan para la tira.
                // Sin ensanchar, wide_dx = 0 y bw == canvas.width exacto
                // (scene_sx = canvas.width / emu_w_), así que el blit es el de
                // siempre hasta el píxel.
                // Sin ancho de frame conocido (`emu_w_ == 0`: FrameView
                // sintética de los oráculos GPU) no hay espacio lógico contra
                // el cual centrar, y el blit va al canvas entero como siempre.
                // Derivarlo igual daba una imagen del tamaño de la textura en
                // un canvas más grande — lo que rompió video_shader_smoke.
                const uint32_t src_w = emu_w_ ? emu_w_ : emu_tex_.width();
                const int32_t  bx = static_cast<int32_t>(wide_dx + 0.5f);
                const int32_t  bw = logical_w
                    ? static_cast<int32_t>(static_cast<float>(src_w) * scene_sx + 0.5f)
                    : static_cast<int32_t>(canvas.width);
                VkImageBlit emu = full_to_rect(src_w,
                                               emu_h_ ? emu_h_ : emu_tex_.height(),
                                               bx, 0, bw,
                                               static_cast<int32_t>(canvas.height));
                blit_tex(cmd, emu_tex_, target_.image(), &emu, 1, VK_FILTER_NEAREST);
            }
            break;
        }
        case AytherLayerKind::TileSubs: {
            // 4. HD tile substitutions -> offscreen regions (LINEAR), batched by texture
            //    (sort by VkTexture* so repeats of the same tile share one transition).
            //    Skipped in Original mode (hd_on=false) — emu frame only.
            if (L.visible && hd_on && pack && fv.tile_sub_count > 0) {
                // Mismo sistema de coordenadas que el blit del emu: las dims del
                // FRAME actual (el fb cambia de modo de video), no las de emu_tex_.
                const float sx = static_cast<float>(canvas.width) /
                                 static_cast<float>(emu_w_ ? emu_w_ : kEmuW);
                const float sy = static_cast<float>(canvas.height) /
                                 static_cast<float>(emu_h_ ? emu_h_ : kEmuH);

                auto& items = scratch_->tile_items;
                items.clear();

                for (uint32_t i = 0; i < fv.tile_sub_count; ++i) {
                    VkTexture* tex = tile_cache_.get_or_load(fv.tile_subs[i].asset_path, pack, ctx, cmd);
                    if (!tex) continue;
                    const int32_t dx = static_cast<int32_t>(fv.tile_subs[i].tile_x * 8u * sx);
                    const int32_t dy = static_cast<int32_t>(fv.tile_subs[i].tile_y * 8u * sy);
                    const int32_t dw = static_cast<int32_t>(8u * sx + 0.5f);
                    const int32_t dh = static_cast<int32_t>(8u * sy + 0.5f);
                    items.push_back({ tex, full_to_rect(tex->width(), tex->height(), dx, dy, dw, dh) });
                }

                std::sort(items.begin(), items.end(),
                          [](const FrameScratch::TileItem& a,
                             const FrameScratch::TileItem& b) { return a.tex < b.tex; });

                auto& group = scratch_->tile_group;
                size_t j = 0;
                while (j < items.size()) {
                    VkTexture* tex = items[j].tex;
                    group.clear();
                    while (j < items.size() && items[j].tex == tex)
                        group.push_back(items[j++].region);
                    blit_tex(cmd, *tex, target_.image(),
                             group.data(), static_cast<uint32_t>(group.size()), VK_FILTER_LINEAR);
                }
            }
            break;
        }
        case AytherLayerKind::Video: {
            // 4V. VIDEO de la Cinemática () — misma ranura que 4Q y por delante de
            //     ella: son MUTUAMENTE EXCLUYENTES por construcción (la sesión emite el
            //     quad del Cuadro o los píxeles del video, nunca los dos — ver el
            //     enrutado del `pick` en ayther_session.cpp). Hereda la semántica
            //     declarada abajo para 4Q: cubre el 100% y es opaco, así que la Utilería
            //     y los glifos autorados sobre esa misma pantalla se siguen dibujando
            //     ENCIMA.
            // : con escena compuesta el video ya se dibujó INLINE en el
            //     pase del plano más alto de su máscara (arriba). Acá queda
            //     sólo el FALLBACK del híbrido, igual que 4R — y sólo cuando
            //     NO hay máscara conocida, porque sin ella no hay dónde
            //     ponerlo y pantalla completa es la degradación de siempre.
            if ((!scene_composed || !fv.video_plane_mask) &&
                L.visible && hd_on && sprite_ok_ && fv.video_y && fv.video_u && fv.video_v && fv.video_w && fv.video_h) {
                sprite_.set_dim(dim_bg, op_bg);
                sprite_.draw_video(ctx, cmd, target_.image(),
                                   static_cast<const uint8_t*>(fv.video_y), fv.video_y_stride,
                                   static_cast<const uint8_t*>(fv.video_u), fv.video_u_stride,
                                   static_cast<const uint8_t*>(fv.video_v), fv.video_v_stride,
                                   fv.video_w, fv.video_h, fv.video_seq);
            }
            break;
        }
        case AytherLayerKind::Picture: {
            // 4Q. CUADRO (CU001) — pantalla estática completa reemplazada por UN asset.
            //     Va ANTES de 4P a propósito: el Cuadro es opaco y cubre el 100%, así
            //     que una Utilería o un glifo autorado sobre esa misma pantalla se
            //     sigue dibujando ENCIMA (el menú cuyo texto cambia). Y como cubre
            //     todo, no hace falta suprimir los tiles originales — se evita la
            //     latencia de 1 frame del canal de supresión.
            if (L.visible && hd_on && sprite_ok_ && fv.screen_sub_count > 0) {
                sprite_.set_dim(dim_bg, op_bg);
                sprite_.draw(ctx, cmd, target_.image(),
                             fv.screen_subs, fv.screen_sub_count, pack,
                             logical_w ? logical_w : kEmuW, emu_h_ ? emu_h_ : kEmuH);
            }
            break;
        }
        case AytherLayerKind::Panorama: {
            // 4R. PANORÁMICA (CU003) — con escena compuesta los quads ya se
            //     dibujaron INLINE en el pase del plano de la tira (el z del
            //     plano que reemplazan — en esta lane global tapaban A/Window/
            //     Sprites, reporte 2026-07-30). Acá queda sólo el FALLBACK del
            //     híbrido (frame sucio/sin escena): sobre el blit, aproximado.
            if (!scene_composed &&
                L.visible && hd_on && sprite_ok_ && fv.panorama_sub_count > 0) {
                sprite_.set_dim(dim_bg, op_bg);
                //  fase 0: la tira ya viene en espacio LÓGICO desde la
                // sesión (su rect trae el centrado sumado), igual que en el
                // camino inline. Dejar puesto el offset de los quads nativos la
                // corre el centrado DOS VECES — 39 px de más con ancho 398, que
                // a ojo se lee como «la Panorámica está mal anclada» y no como
                // un defecto del ensanchado.
                sprite_.set_wide_offset(0.0f);
                sprite_.draw(ctx, cmd, target_.image(),
                             fv.panorama_subs, fv.panorama_sub_count, pack,
                             logical_w ? logical_w : kEmuW, emu_h_ ? emu_h_ : kEmuH,
                             nullptr, fv.panorama_sub_tint);   // fundido E1
                sprite_.set_wide_offset(wide_dx_emu);
            }
            break;
        }
        case AytherLayerKind::PlaneTilesLo: {
            // 4P. HD plane-tile overlay (Fase 2c) -> reusa el pase de sprites (quads 1×1
            //     pixel-precisos con alpha), resueltos scroll-aware por AytherSession. Los
            //     de BAJA prioridad ([0,hi)) van ANTES de los sprites → fondo HD DEBAJO de
            //     los sprites. `plane_tile_flips` vuelca la variante volteada del patrón.
            //     SIN gate de pack: VkSprite::draw cae al disco para assets sueltos, así
            //     la autoría en Editar previsualiza el fondo HD antes de construir un pack.
            if (L.visible && hd_on && sprite_ok_ && fv.plane_tile_sub_hi > 0) {
                sprite_.set_dim(1.0f);   // el atenuado va POR SUB (tinte + alpha)
                sprite_.draw(ctx, cmd, target_.image(),
                             fv.plane_tile_subs, fv.plane_tile_sub_hi, pack,
                             logical_w ? logical_w : kEmuW, emu_h_ ? emu_h_ : kEmuH,
                             fv.plane_tile_flips,
                             plane_tile_tint.empty() ? nullptr
                                                     : plane_tile_tint.data(),
                             nullptr,   // slots: los tiles no se solapan por SAT
                             plane_tile_alpha.empty() ? nullptr
                                                      : plane_tile_alpha.data());
            }
            break;
        }
        case AytherLayerKind::SpritesHd: {
            // 4S. HD sprite overlay -> the offscreen (alpha-blended over emu+tiles).
            //     VkSprite::draw barriers TRANSFER_DST->COLOR_ATTACHMENT, runs its render
            //     pass (loadOp=LOAD), and leaves the offscreen back in TRANSFER_DST.
            //     Skipped in Original mode (hd_on=false). SIN gate de pack (igual que el
            //     pase de tiles de plano 4P): VkSprite::draw cae al disco para los assets
            //     de las asignaciones sueltas (assign_sprite con ruta de disco) → la
            //     preview HD de una pose se ve en vivo en Animar/Editar sin construir un
            //     pack. Los subs de pack (rutas del .ay) sólo aparecen con el pack cargado.
            if (L.visible && hd_on && sprite_ok_ && fv.sprite_sub_count > 0) {
                sprite_.set_dim(dim_spr, op_spr);
                if (scene_composed) {
                    // R-5 2b: los subs con ancla los resuelve la escena (los
                    // dibuja en el z de la cadena, o los OMITE si el ancla está
                    // oculta); acá sólo el resto (sin ancla — slot 255 /
                    // entity-like) para no duplicarlos encima.
                    for (uint32_t si = 0; si < fv.sprite_sub_count; ++si) {
                        if (si < sub_has_anchor.size() && sub_has_anchor[si])
                            continue;
                        sprite_.draw(ctx, cmd, target_.image(),
                                     fv.sprite_subs + si, 1, pack,
                                     logical_w ? logical_w : kEmuW, emu_h_ ? emu_h_ : kEmuH,
                                     fv.sprite_sub_flips ? fv.sprite_sub_flips + si : nullptr,
                                     fv.sprite_sub_tint  ? fv.sprite_sub_tint + si * 3 : nullptr,
                                     fv.sprite_sub_slot  ? fv.sprite_sub_slot + si : nullptr);
                    }
                } else {
                    // HÍBRIDO: los subs pri-1 NO van acá — el hardware los
                    // dibuja DELANTE del plano de alta prioridad (sp1 > A1:
                    // las letras del título de GA sobre el isologotipo), así
                    // que se difieren a después de la lane PlaneTilesHi.
                    for (uint32_t si = 0; si < fv.sprite_sub_count; ++si) {
                        if (fv.sprite_sub_prio && fv.sprite_sub_prio[si])
                            continue;
                        sprite_.draw(ctx, cmd, target_.image(),
                                     fv.sprite_subs + si, 1, pack,
                                     logical_w ? logical_w : kEmuW, emu_h_ ? emu_h_ : kEmuH,
                                     fv.sprite_sub_flips ? fv.sprite_sub_flips + si : nullptr,
                                     fv.sprite_sub_tint  ? fv.sprite_sub_tint + si * 3 : nullptr,
                                     fv.sprite_sub_slot  ? fv.sprite_sub_slot + si : nullptr);
                    }
                }
            }
            break;
        }
        case AytherLayerKind::Mode3: {
            // 4M. Modo 3: sustitución HD POR INSTANCIA (RAM anchoring) — misma lane que los
            //     sprites (VkSprite::draw), un quad por entidad anclada con asset, sobre los
            //     per-sprite subs (la asignación por instancia es la intención más específica).
            //     SIN gate de pack (como 4P): la autoría en vivo asigna archivos sueltos y
            //     VkSprite cae al disco. (: con el compose de Editar activo, los quads
            //     se RE-DIBUJAN después del 4C — ver 4M-bis abajo.)
            if (L.visible && hd_on && sprite_ok_ && fv.entity_sub_count > 0) {
                sprite_.set_dim(dim_spr, op_spr);
                sprite_.draw(ctx, cmd, target_.image(),
                             fv.entity_subs, fv.entity_sub_count, pack,
                             logical_w ? logical_w : kEmuW, emu_h_ ? emu_h_ : kEmuH);
            }
            break;
        }
        case AytherLayerKind::Anim: {
            // 4A. Animaciones C-S2: frames HD de sheet EN FASE con el juego — sub-rect
            //     (UV) del sheet al bbox observado (Pop) o al transform tweeneado (Nivel
            //     1, dst float sub-píxel). Misma lane alpha-blend que los sprites.
            if (L.visible && hd_on && sprite_ok_ && fv.anim_frame_count > 0) {
                sprite_.set_dim(dim_spr, op_spr);
                sprite_.draw_anim(ctx, cmd, target_.image(),
                                  fv.anim_frames, fv.anim_frame_count, pack,
                                  logical_w ? logical_w : kEmuW, emu_h_ ? emu_h_ : kEmuH);
            }
            break;
        }
        case AytherLayerKind::VdpFrente: {
            // R-5 parte 2b: el PRIMER PLANO ORIGINAL — elementos pri-1 de la
            // escena sobre las lanes HD de fondo/sprites. Reemplaza al compose
            // 4C: lo que el VDP dibuja delante (texto, arcos, wipes) queda
            // delante del HD sin doble render ni supresión. Sólo en frames
            // compuestos (el blit del fallback ya trae todo el frame).
            if (L.visible && scene_composed)
                draw_scene_pass(1, -1);
            //  decisión 3: el video del Cuadro pri-1 va ACÁ, después del
            // pase de primer plano — es el «flash a pantalla completa» de una
            // cinemática, y lo que viene a tapar es justamente lo que se acaba
            // de dibujar. Puesto antes del pase quedaría detrás de ello, que es
            // el defecto que la decisión evita.
            if (L.visible && scene_composed && fv.video_front &&
                fv.video_plane_mask && hd_on && sprite_ok_ &&
                fv.video_y && fv.video_u && fv.video_v &&
                fv.video_w && fv.video_h) {
                sprite_.set_dim(1.0f);
                sprite_.draw_video(ctx, cmd, target_.image(),
                                   static_cast<const uint8_t*>(fv.video_y), fv.video_y_stride,
                                   static_cast<const uint8_t*>(fv.video_u), fv.video_u_stride,
                                   static_cast<const uint8_t*>(fv.video_v), fv.video_v_stride,
                                   fv.video_w, fv.video_h, fv.video_seq);
            }
            break;
        }
        case AytherLayerKind::PlaneTilesHi: {
            // 4P-hi. HD plane-tile overlay — ALTA prioridad ([hi,count)): SOBRE los sprites
            //        pri-0 (tiles de Plano A de primer plano que el VDP dibuja delante del
            //        sprite). Con escena compuesta, los que tienen elemento en la cadena ya
            //        se dibujaron INLINE en el pase pri-1 (z real) — acá solo el resto
            //        (fallback, mismo patrón que la Panorámica).
            if (L.visible && hd_on && sprite_ok_ &&
                fv.plane_tile_sub_count > fv.plane_tile_sub_hi) {
                sprite_.set_dim(1.0f);
                if (scene_composed) {
                    for (uint32_t pi = fv.plane_tile_sub_hi;
                         pi < fv.plane_tile_sub_count; ++pi) {
                        if (pi < plane_hi_anchor.size() && plane_hi_anchor[pi])
                            continue;
                        sprite_.draw(ctx, cmd, target_.image(),
                                     fv.plane_tile_subs + pi, 1, pack,
                                     logical_w ? logical_w : kEmuW, emu_h_ ? emu_h_ : kEmuH,
                                     fv.plane_tile_flips + pi,
                                     plane_tile_tint.empty()
                                         ? nullptr
                                         : plane_tile_tint.data() + (size_t)pi * 3);
                    }
                } else {
                    sprite_.draw(ctx, cmd, target_.image(),
                                 fv.plane_tile_subs + fv.plane_tile_sub_hi,
                                 fv.plane_tile_sub_count - fv.plane_tile_sub_hi, pack,
                                 logical_w ? logical_w : kEmuW, emu_h_ ? emu_h_ : kEmuH,
                                 fv.plane_tile_flips + fv.plane_tile_sub_hi,
                                 plane_tile_tint.empty()
                                     ? nullptr
                                     : plane_tile_tint.data() +
                                           (size_t)fv.plane_tile_sub_hi * 3);
                }
            }
            // HÍBRIDO: los sprites HD pri-1 diferidos de la lane SpritesHd —
            // el hardware los pone DELANTE del plano de alta prioridad. Van
            // con el atenuado y la visibilidad de SU lane (son sprites).
            if (!scene_composed && spr_lane_visible && hd_on && sprite_ok_ &&
                fv.sprite_sub_count > 0 && fv.sprite_sub_prio) {
                sprite_.set_dim(dim_spr, op_spr);
                for (uint32_t si = 0; si < fv.sprite_sub_count; ++si) {
                    if (!fv.sprite_sub_prio[si]) continue;
                    sprite_.draw(ctx, cmd, target_.image(),
                                 fv.sprite_subs + si, 1, pack,
                                 logical_w ? logical_w : kEmuW, emu_h_ ? emu_h_ : kEmuH,
                                 fv.sprite_sub_flips ? fv.sprite_sub_flips + si : nullptr,
                                 fv.sprite_sub_tint  ? fv.sprite_sub_tint + si * 3 : nullptr,
                                 fv.sprite_sub_slot  ? fv.sprite_sub_slot + si : nullptr);
                }
            }
            break;
        }
        case AytherLayerKind::Custom: {
            // R-7 (): la capa insertada dibuja su LÁMINA tileada en su
            // posición del stack, movida por el ratio de parallax. Scroll:
            // DERIVADO de un plano (ancla B/A × factor — cámara EM-1
            // des-wrapeada si el tracking es secuencial; si no, el H wrapeado
            // del frame, mismo signo) o DECLARADO contra la cámara de nivel.
            // hd_on: el Acetato es contenido HD AGREGADO — en modo Original no
            // se dibuja, como toda lane HD. Antes lo ignoraba y con GATE por
            // Cuadro quedaba inconsistente: el matcher solo corre con HD ON,
            // así que el acetato gateado desaparecía y el global no (reporte
            // «no aparece todas las veces», 2026-08-18).
            if (!hd_on) break;
            const AytherLayerContent& cc = L.content;
            if (!L.visible || !cc.asset[0] || !cc.img_w || !cc.img_h || !sprite_ok_)
                break;
            // : condición de aparición — atado a un Cuadro, el Acetato
            // existe sólo mientras ese Cuadro matchea. El matcher ya trae su
            // propia estabilidad (score/histéresis); acá no se agrega otra.
            // : con VARIOS Cuadros, abre si matchea CUALQUIERA (exacto
            // contra screen_match_id; presencia contra los presentes).
            if (!overlay_gate_open(cc, fv.screen_match_id, fv.screen_presence_ids,
                                   fv.screen_presence_count))
                break;
            // : el paso vigente de la animación — función pura del frame
            // de la toma (seek atrás y pausa dan el mismo cuadro). Un paso sin
            // lámina declarada cae al paso 0, nunca a un quad vacío.
            const uint32_t astep = overlay_animation_step(
                fv.frame_index, cc.anim_ticks, cc.anim_count);
            const char* use_asset = astep == 0 ? cc.asset : cc.anim[astep - 1];
            if (!use_asset[0]) use_asset = cc.asset;
            float h = 0.f;
            if (cc.anchor <= 1) {
                const int plane = cc.anchor == 0 ? 1 : 0;   // 0=B → hscroll[1] · 1=A → [0]
                // : con patrón (tile_mode 1) la posición sólo importa MOD
                // img_w y el hscroll CRUDO del frame es exacto (mismo criterio
                // que el término vertical). La cámara EM-1 des-wrapeada
                // arrastra en VIVO el residuo de las pantallas anteriores
                // (logo→demos→título) y correría la costura del wrap de la
                // lámina al medio del cielo. Fila única sigue con la cámara:
                // una lámina más larga que el wrap de 512 necesita recorrido.
                if (cc.tile_mode == 1)
                    h = (float)fv.plane_hscroll[plane];
                else
                    h = fv.plane_cam_valid ? -(float)fv.plane_cam_x[plane]
                                           : (float)fv.plane_hscroll[plane];
            } else {
                // declarado: ratio propio contra la cámara de nivel (plano A).
                h = fv.plane_cam_valid ? -(float)fv.plane_cam_x[0]
                                       : (float)fv.plane_hscroll[0];
            }
            // : deriva propia en px/s, SUMADA al offset de parallax. El
            // término temporal es función PURA del frame de la toma (frame/fps
            // de timing) — nunca reloj de pared ni acumulador: seek atrás y
            // pausa tienen que dar el mismo píxel (replay byte-exacto; el
            // precedente del tint rancio de la Panorámica y el subdesborde del
            // AnimationPlayer son exactamente este bug en otras capas).
            const double t = fv.fps_timing > 1.0
                ? (double)fv.frame_index / fv.fps_timing : 0.0;
            float off = std::fmod(h * cc.factor + (float)(cc.drift_x * t),
                                  (float)cc.img_w);
            if (off > 0.f) off -= (float)cc.img_w;
            auto& strip = scratch_->overlay_strip;
            strip.clear();
            const int ew = (int)(emu_w_ ? emu_w_ : kEmuW);
            const int eh = (int)(emu_h_ ? emu_h_ : kEmuH);
            // : fila única a `y` (default, ni un píxel distinto de hoy) o
            // tileado en X e Y desde el offset vertical (cubrir la pantalla con
            // un patrón sin exigir un PNG tan alto como el canvas). El drift_y
            // en fila única mueve la fila; en 2D rota el patrón (wrap).
            // Anclaje VERTICAL (reporte 2026-08-18: el cielo del título
            // scrollea en Y toda la toma y el acetato quedaba fijo). Misma
            // convención del VDP que el término horizontal: la línea y muestra
            // la fila (y + vscroll) del plano => el contenido sube cuando
            // vscroll crece => la lámina anclada RESTA el vscroll. `factor`
            // escala ambos ejes (es el ratio del ancla, no un parámetro
            // horizontal).
            const int vplane = (cc.anchor == 0) ? 1 : 0;
            const float vanchor = -(float)fv.plane_vscroll[vplane] * cc.factor;
            float y0 = (float)cc.y + vanchor + (float)(cc.drift_y * t);
            if (cc.tile_mode == 1) {
                y0 = std::fmod(y0, (float)cc.img_h);
                if (y0 > 0.f) y0 -= (float)cc.img_h;
            }
            // Guarda del caso patológico (): con img chico y tileado 2D los
            // quads crecen por producto — tope + log al recortar, nunca en
            // silencio (mismo criterio que kMaxPlaneSetMembers).
            constexpr size_t kMaxOverlayQuads = 512;
            bool clipped = false;
            // AJUSTE A PANTALLA: un solo quad estirado al canvas visible del
            // frame — GA conmuta H32/H40 y el marco de 320 quedaba cortado a
            // la derecha en el título de 256. Ignora anclaje/tile/deriva.
            if (cc.fit) {
                AytherSpriteSub sub{};
                std::snprintf(sub.asset_path, sizeof(sub.asset_path), "%s",
                              use_asset);
                sub.screen_x = 0; sub.screen_y = 0;
                sub.w_tiles  = (uint8_t)std::min(255, (ew + 7) / 8);
                sub.h_tiles  = (uint8_t)std::min(255, (eh + 7) / 8);
                sub.w_px     = (uint16_t)ew;
                sub.h_px     = (uint16_t)eh;
                sub.palette  = 0xFF;
                sub.synth_pal = 0xFF;
                sub.uw = 1.f; sub.vh = 1.f;
                strip.push_back(sub);
            } else {
            //  fidelidad estricta: en modo VS (vscroll por columna, reg 11
            // bit 2) el plano ancla scrollea en tiras de 16 px con fase propia
            // — las nubes del título de GA son 16 tiras a dos velocidades. El
            // acetato anclado dibuja POR TIRA: cada una recorta su banda de la
            // lámina (sub-rect UV) y la desplaza con SU columna de VSRAM.
            // Fuera del modo VS, o con anclaje declarado, el camino de siempre
            // (byte-exacto para los oráculos).
            const bool percol = fv.vs_two_cell && cc.anchor <= 1;
            if (!percol) {
                for (float y = y0;;) {
                    for (float x = off; x < (float)ew; x += (float)cc.img_w) {
                        if (strip.size() >= kMaxOverlayQuads) { clipped = true; break; }
                        AytherSpriteSub sub{};
                        std::snprintf(sub.asset_path, sizeof(sub.asset_path), "%s",
                                      use_asset);
                        sub.screen_x = (int16_t)std::lround(x);
                        sub.screen_y = (int16_t)std::lround(y);
                        sub.w_tiles  = (uint8_t)std::min(255, (cc.img_w + 7) / 8);
                        sub.h_tiles  = (uint8_t)std::min(255, (cc.img_h + 7) / 8);
                        sub.w_px     = cc.img_w;
                        sub.h_px     = cc.img_h;
                        sub.palette  = 0xFF;   // sin ancla E1 (no tinta con la CRAM)
                        sub.synth_pal = 0xFF;
                        sub.uw = 1.f; sub.vh = 1.f;   // quad completo (u0/v0 = 0)
                        strip.push_back(sub);
                    }
                    y += (float)cc.img_h;
                    if (clipped || cc.tile_mode != 1 || y >= (float)eh) break;
                }
            } else {
                for (int s = 0; s * 16 < ew && !clipped; ++s) {
                    const float sx0 = (float)(s * 16);
                    const float sx1 = (float)std::min(s * 16 + 16, ew);
                    const int   ci  = std::min(s, 19);
                    // Misma convención que el término global: la lámina
                    // anclada RESTA el vscroll (de SU columna).
                    const float vs = -(float)fv.plane_vscroll_col[vplane][ci]
                                   * cc.factor;
                    float ys = (float)cc.y + vs + (float)(cc.drift_y * t);
                    if (cc.tile_mode == 1) {
                        ys = std::fmod(ys, (float)cc.img_h);
                        if (ys > 0.f) ys -= (float)cc.img_h;
                    }
                    for (float y = ys;;) {
                        // Segmentos: intersección de la tira con los quads que
                        // tilean en X desde `off` (recorte por sub-rect UV).
                        for (float x = off; x < sx1; x += (float)cc.img_w) {
                            const float ix0 = std::max(x, sx0);
                            const float ix1 = std::min(x + (float)cc.img_w, sx1);
                            if (ix1 <= ix0) continue;
                            if (strip.size() >= kMaxOverlayQuads) { clipped = true; break; }
                            AytherSpriteSub sub{};
                            std::snprintf(sub.asset_path, sizeof(sub.asset_path),
                                          "%s", use_asset);
                            sub.screen_x = (int16_t)std::lround(ix0);
                            sub.screen_y = (int16_t)std::lround(y);
                            const int segw = (int)std::lround(ix1 - ix0);
                            sub.w_tiles  = (uint8_t)std::min(255, (segw + 7) / 8);
                            sub.h_tiles  = (uint8_t)std::min(255, (cc.img_h + 7) / 8);
                            sub.w_px     = (uint16_t)segw;
                            sub.h_px     = cc.img_h;
                            sub.palette  = 0xFF;
                            sub.synth_pal = 0xFF;
                            sub.u0 = (ix0 - x) / (float)cc.img_w;
                            sub.uw = (ix1 - ix0) / (float)cc.img_w;
                            sub.vh = 1.f;
                            strip.push_back(sub);
                        }
                        y += (float)cc.img_h;
                        if (clipped || cc.tile_mode != 1 || y >= (float)eh) break;
                    }
                }
            }
            }   // fin del camino anclado/tileado (cc.fit lo saltea entero)
            if (clipped) {
                static uint64_t warned = 0;   // 1.ª vez y luego cada ~10 s clippeados
                if (warned++ % 600 == 0)
                    ayther::log::write(ayther::log::Severity::Warning,
                        "renderer", "acetato_tileado_d_recortado",
                        "Acetato \"%s\": tileado 2D"
                                 " recortado a %zu quads (img %ux%u sobre %dx%d)",
                        cc.asset,
                        kMaxOverlayQuads,
                        cc.img_w,
                        cc.img_h,
                        ew,
                        eh);
            }
            if (!strip.empty()) {
                // : opacidad por-quad (reusa el canal de alphas de ) y
                // modo de mezcla por llamada. Neutro (opacity=1, blend=0) pasa
                // nullptr/0 → exactamente la ruta de siempre, byte-exacta.
                // Limitación anotada: con opacity<1 el alpha por-quad REEMPLAZA
                // la opacidad de set_dim (atenuado de capa enfocada), no la
                // compone — mismo contrato que los tiles de plano.
                float op = cc.opacity < 0.f ? 0.f
                         : cc.opacity > 1.f ? 1.f : cc.opacity;
                // : parpadeo determinista — función PURA del frame de la
                // toma (mismo contrato que la deriva  y el paso :
                // seek atrás y pausa dan el mismo píxel). amp 0 = apagado.
                if (cc.flicker_amp > 0.f) {
                    const uint32_t fstep = cc.flicker_ticks
                        ? fv.frame_index / cc.flicker_ticks : fv.frame_index;
                    op *= 1.f - (cc.flicker_amp < 1.f ? cc.flicker_amp : 1.f)
                              * overlay_flicker_factor(fstep);
                }
                auto& alphas = scratch_->overlay_alphas;
                const uint8_t* ap = nullptr;
                if (op < 1.f) {
                    alphas.assign(strip.size(),
                                  (uint8_t)std::lround(op * 255.f));
                    ap = alphas.data();
                }
                // : tinte E1 de la lámina — razón live/ref POR CANAL de la
                // línea CRAM ancla, la misma fórmula de las poses (Q2.6, 64 =
                // neutro). Sin CRAM de escena (core stock, frame no compuesto)
                // no hay dato: se dibuja sin tinte, como siempre. El promedio
                // salta el índice 0 (transparente), igual que el peak-hold.
                auto& tintq = scratch_->overlay_tint;
                const uint8_t* tp = nullptr;
                if (cc.pal_line < 4 &&
                    (cc.ref_rgb[0] | cc.ref_rgb[1] | cc.ref_rgb[2]) &&
                    fv.scene_cram &&
                    fv.scene_cram_size >= (size_t)(cc.pal_line + 1) * 32) {
                    double live[3] = { 0, 0, 0 };
                    // : sólo los índices de la máscara (los que la lámina
                    // usa) — el resto de la línea diluye la razón live/ref.
                    const uint16_t tmask =
                        cc.tint_mask ? cc.tint_mask : (uint16_t)0xFFFE;
                    int tn = 0;
                    for (int i = 0; i < 16; ++i) {
                        if (!(tmask & (1u << i))) continue;
                        const size_t o = (size_t)(cc.pal_line * 16 + i) * 2;
                        const uint16_t v = (uint16_t)(fv.scene_cram[o] |
                                                      (fv.scene_cram[o + 1] << 8));
                        live[0] += (v     ) & 7;
                        live[1] += (v >> 3) & 7;
                        live[2] += (v >> 6) & 7;
                        ++tn;
                    }
                    uint8_t q[3];
                    bool neutral = true;
                    for (int c2 = 0; c2 < 3; ++c2) {
                        const double l255 =
                            live[c2] * 255.0 / ((double)(tn ? tn : 1) * 7.0);
                        const double t = l255 /
                            (double)(cc.ref_rgb[c2] ? cc.ref_rgb[c2] : 1);
                        const double qq = t * 64.0 + 0.5;
                        q[c2] = (uint8_t)(qq < 0.0 ? 0.0 : qq > 255.0 ? 255.0 : qq);
                        if (q[c2] != 64) neutral = false;
                    }
                    if (!neutral) {
                        tintq.assign(strip.size() * 3, 64);
                        for (size_t k = 0; k < strip.size(); ++k) {
                            tintq[k * 3 + 0] = q[0];
                            tintq[k * 3 + 1] = q[1];
                            tintq[k * 3 + 2] = q[2];
                        }
                        tp = tintq.data();
                    }
                }
                sprite_.draw(ctx, cmd, target_.image(), strip.data(),
                             (uint32_t)strip.size(), pack,
                             logical_w ? logical_w : kEmuW, emu_h_ ? emu_h_ : kEmuH,
                             nullptr, tp, nullptr, ap, cc.blend);
            }
            break;
        }
        }
    }

    // 5. Offscreen TRANSFER_DST -> SHADER_READ_ONLY: the frontend samples it
    //    (CRT post-process, or the Lab viewport in R3.3) or blits it to the
    //    swapchain (blit_to_swapchain transitions SHADER_READ -> TRANSFER_SRC).
    image_barrier(cmd, target_.image(),
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

void AytherRenderer::copy_target_to_buffer(VkCommandBuffer cmd, VkBuffer dst) {
    // Readback del export MP4: el offscreen quedó SHADER_READ_ONLY tras
    // render() — barrera a TRANSFER_SRC, copia tightly-packed BGRA al buffer
    // host-visible del caller, y de vuelta a SHADER_READ_ONLY (el viewport del
    // Lab puede seguir sampleándolo mientras dura el export).
    const VkExtent2D e = target_.extent();
    image_barrier(cmd, target_.image(),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy r{};
    r.bufferOffset      = 0;
    r.bufferRowLength   = 0;   // 0 = tightly packed (== width)
    r.bufferImageHeight = 0;
    r.imageSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    r.imageOffset       = { 0, 0, 0 };
    r.imageExtent       = { e.width, e.height, 1 };
    vkCmdCopyImageToBuffer(cmd, target_.image(),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &r);
    image_barrier(cmd, target_.image(),
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

// ---------------------------------------------------------------------------
// : captura de comparación — una COPIA del offscreen para el A/B.
//
// El offscreen quedó SHADER_READ_ONLY tras render(). Se lo pasa a TRANSFER_SRC,
// la copia a TRANSFER_DST, se copia imagen a imagen (mismo formato y extent, sin
// conversión ni escalado) y las dos vuelven a SHADER_READ_ONLY: el viewport
// puede samplear las dos en el mismo frame de UI, que es todo el punto.
// ---------------------------------------------------------------------------
bool AytherRenderer::capture_compare(const ayther::engine::VulkanContextView& ctx, VkCommandBuffer cmd) {
    if (!target_.is_ready()) return false;
    const VkExtent2D e = target_.extent();
    // Lazy + resize: el canvas cambia con el zoom y con el tier de export.
    if (!compare_.is_ready() || compare_.extent().width  != e.width
                             || compare_.extent().height != e.height) {
        if (compare_.is_ready()) compare_.shutdown(ctx);
        if (!compare_.init(ctx, e.width, e.height, target_.format()))
            return false;
    }
    image_barrier(cmd, target_.image(),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    // La copia recién creada está en UNDEFINED; una ya usada, en SHADER_READ_ONLY.
    // Descartar el contenido viejo es correcto: se la va a sobrescribir entera.
    image_barrier(cmd, compare_.image(),
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        0, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageCopy r{};
    r.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    r.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    r.extent         = { e.width, e.height, 1 };
    vkCmdCopyImage(cmd, target_.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   compare_.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    image_barrier(cmd, target_.image(),
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    image_barrier(cmd, compare_.image(),
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    return true;
}

void AytherRenderer::compare_release(const ayther::engine::VulkanContextView& ctx) {
    if (compare_.is_ready()) compare_.shutdown(ctx);
}

// ---------------------------------------------------------------------------
// Readback del export MP4 — recursos propios (no se ata al present).
// ---------------------------------------------------------------------------
bool AytherRenderer::readback_init(const ayther::engine::VulkanContextView& ctx) {
    readback_shutdown(ctx);
    const VkExtent2D e = target_.extent();

    VkCommandPoolCreateInfo pi{};
    pi.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                          VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = ctx.graphics_family();
    if (vkCreateCommandPool(ctx.device(), &pi, nullptr, &rb_pool_) != VK_SUCCESS)
        return false;
    VkCommandBufferAllocateInfo ai{};
    ai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool        = rb_pool_;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(ctx.device(), &ai, &rb_cmd_) != VK_SUCCESS) {
        readback_shutdown(ctx);
        return false;
    }
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(ctx.device(), &fi, nullptr, &rb_fence_) != VK_SUCCESS) {
        readback_shutdown(ctx);
        return false;
    }
    // Buffer host-visible con mapeo persistente — molde del staging de
    // VkTexture::init, pero HOST_ACCESS_RANDOM (la CPU LEE) y TRANSFER_DST.
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size  = static_cast<VkDeviceSize>(e.width) * e.height * 4;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo aci{};
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT;
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(ctx.allocator(), &bi, &aci, &rb_buf_, &rb_alloc_, &info)
            != VK_SUCCESS) {
        ayther::log::write(ayther::log::Severity::Error,
            "renderer", "readback_buffer_x_failed",
            "readback buffer (%ux%u) failed",
            e.width,
            e.height);
        readback_shutdown(ctx);
        return false;
    }
    rb_map_ = info.pMappedData;
    ayther::log::write(ayther::log::Severity::Info,
        "renderer", "readback_listo_x_mb",
        "readback listo %ux%u (%zu MB)",
        e.width,
        e.height,
        static_cast<size_t>(bi.size) >> 20);
    return rb_map_ != nullptr;
}

const uint8_t* AytherRenderer::export_frame(const ayther::engine::VulkanContextView& ctx, const FrameView& fv,
                                            AyArchive* pack, bool hd_on,
                                            const AytherLayerStack* layers,
                                            uint8_t vdp_mask) {
    if (!rb_map_) return nullptr;
    vkResetCommandBuffer(rb_cmd_, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(rb_cmd_, &bi) != VK_SUCCESS) return nullptr;
    render(ctx, rb_cmd_, fv, pack, hd_on, layers, vdp_mask);
    copy_target_to_buffer(rb_cmd_, rb_buf_);
    vkEndCommandBuffer(rb_cmd_);

    VkSubmitInfo si{};
    si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &rb_cmd_;
    if (vkQueueSubmit(ctx.graphics_queue(), 1, &si, rb_fence_) != VK_SUCCESS)
        return nullptr;
    vkWaitForFences(ctx.device(), 1, &rb_fence_, VK_TRUE, UINT64_MAX);
    vkResetFences(ctx.device(), 1, &rb_fence_);
    // La memoria puede no ser HOST_COHERENT: invalidar antes de leer.
    vmaInvalidateAllocation(ctx.allocator(), rb_alloc_, 0, VK_WHOLE_SIZE);
    return static_cast<const uint8_t*>(rb_map_);
}

const uint8_t* AytherRenderer::readback_compare(const ayther::engine::VulkanContextView& ctx) {
    if (!rb_map_ || !compare_.is_ready()) return nullptr;
    vkResetCommandBuffer(rb_cmd_, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(rb_cmd_, &bi) != VK_SUCCESS) return nullptr;
    // La copia quedó SHADER_READ_ONLY tras capture_compare: mismo ida y vuelta
    // que copy_target_to_buffer, sobre la otra imagen.
    const VkExtent2D e = compare_.extent();
    image_barrier(rb_cmd_, compare_.image(),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy r{};
    r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    r.imageExtent      = { e.width, e.height, 1 };
    vkCmdCopyImageToBuffer(rb_cmd_, compare_.image(),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb_buf_, 1, &r);
    image_barrier(rb_cmd_, compare_.image(),
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    vkEndCommandBuffer(rb_cmd_);

    VkSubmitInfo si{};
    si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &rb_cmd_;
    if (vkQueueSubmit(ctx.graphics_queue(), 1, &si, rb_fence_) != VK_SUCCESS)
        return nullptr;
    vkWaitForFences(ctx.device(), 1, &rb_fence_, VK_TRUE, UINT64_MAX);
    vkResetFences(ctx.device(), 1, &rb_fence_);
    vmaInvalidateAllocation(ctx.allocator(), rb_alloc_, 0, VK_WHOLE_SIZE);
    return static_cast<const uint8_t*>(rb_map_);
}

/// : captura para el oráculo — graba capture_compare en el cmd de readback
/// y lo submitea. En el Lab la captura viaja en el cmd del frame; acá no hay
/// frame, así que necesita su propio submit.
bool AytherRenderer::capture_compare_now(const ayther::engine::VulkanContextView& ctx) {
    if (!rb_cmd_) return false;
    vkResetCommandBuffer(rb_cmd_, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(rb_cmd_, &bi) != VK_SUCCESS) return false;
    const bool ok = capture_compare(ctx, rb_cmd_);
    vkEndCommandBuffer(rb_cmd_);
    if (!ok) return false;
    VkSubmitInfo si{};
    si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &rb_cmd_;
    if (vkQueueSubmit(ctx.graphics_queue(), 1, &si, rb_fence_) != VK_SUCCESS)
        return false;
    vkWaitForFences(ctx.device(), 1, &rb_fence_, VK_TRUE, UINT64_MAX);
    vkResetFences(ctx.device(), 1, &rb_fence_);
    return true;
}

void AytherRenderer::readback_shutdown(const ayther::engine::VulkanContextView& ctx) {
    if (rb_buf_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(ctx.allocator(), rb_buf_, rb_alloc_);
        rb_buf_ = VK_NULL_HANDLE; rb_alloc_ = VK_NULL_HANDLE; rb_map_ = nullptr;
    }
    if (rb_fence_ != VK_NULL_HANDLE) {
        vkDestroyFence(ctx.device(), rb_fence_, nullptr);
        rb_fence_ = VK_NULL_HANDLE;
    }
    if (rb_pool_ != VK_NULL_HANDLE) {   // libera también el cmd alocado
        vkDestroyCommandPool(ctx.device(), rb_pool_, nullptr);
        rb_pool_ = VK_NULL_HANDLE; rb_cmd_ = VK_NULL_HANDLE;
    }
}

}  // namespace ayther
