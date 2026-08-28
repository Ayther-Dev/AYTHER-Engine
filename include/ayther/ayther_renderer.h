#pragma once
// ---------------------------------------------------------------------------
// AytherRenderer — the motor's visual (HD) layer (R3).
//
// Consumes a FrameView (the deterministic CPU output of AytherSession::step())
// + a borrowed VkContext, and renders the HD frame into an offscreen VkImage
// (VkRenderTarget). The frontend then presents that image:
//   - ayther_play blits it to its swapchain;
//   - ayther_lab samples it in an ImGui viewport.
//
// Kept SEPARATE from AytherSession on purpose: the session stays Vulkan-free /
// headless (CI, determinism, future mobile); the renderer is the swappable GPU
// layer. See docs/architecture/r3-render-to-texture.md.
//
// Lifecycle: init(ctx, w, h) → render(ctx, cmd, fv) per frame → shutdown(ctx).
// Single-owner; driven from the same thread as the session.
//
// R3.0: scaffold — owns the offscreen target; render() clears it. The emu-frame,
// HD-tile, sprite and post-process passes land in R3.1 / R3.2.
// ---------------------------------------------------------------------------
#include <vulkan/vulkan.h>
#include <cstdint>

#include "vulkan_backend/vk_render_target.h"
#include "vulkan_backend/vk_texture.h"        // emu-frame texture
#include "vulkan_backend/tile_tex_cache.h"    // HD tile textures
#include "vulkan_backend/vk_sprite.h"         // HD sprite overlay (R3.2)
#include "vulkan_backend/vk_indexed_plane.h"  // R-5 (): compose indexado

#include <string>

struct AyArchive;   // opaque (ayther_core_ffi.h) — HD asset source
class  VkContext;
class  AytherLayerStack;   // R-4 () — modelo de capas (ayther_layers.h)

namespace ayther {

struct FrameView;   // ayther_session.h (full definition needed only in the .cpp)
struct SceneElement;   // ayther_session.h — R-8: sub_texture_state()

/// @brief Vulkan presentation layer for one session frame stream.
///
/// The renderer borrows `VkContext`; the context and its device must outlive
/// every renderer resource. All methods are render-thread affine and are not
/// safe for concurrent use. shutdown() is mandatory before destroying the
/// borrowed context, including after partial initialization.
class AytherRenderer {
public:
    AytherRenderer()  = default;
    ~AytherRenderer() = default;   // shutdown() must be called explicitly

    AytherRenderer(const AytherRenderer&)            = delete;
    AytherRenderer& operator=(const AytherRenderer&) = delete;

    // Create the offscreen target + the render pipelines at the canvas
    // resolution. ayther_play passes its swapchain extent; ayther_lab its
    // viewport size. `shader_dir` locates the SPIR-V (sprite.*.spv); if the
    // shaders are missing the sprite overlay is disabled (emu+tiles still work).
    bool init(VkContext& ctx, uint32_t canvas_w, uint32_t canvas_h,
              const char* shader_dir);

    // Recreate at a new canvas size (window / viewport resize).
    bool resize(VkContext& ctx, uint32_t canvas_w, uint32_t canvas_h);

    // Evict cached HD tile textures so the next frame re-fetches from the new
    // pack. Call on pack hot-reload, after vkDeviceWaitIdle().
    void evict_pack_textures(VkContext& ctx);

    // Evict UNA textura de sprite HD por asset path (incluye sus variantes
    // volteadas) para que el próximo frame la recargue del pack/disco. Para
    // assets reescritos en vivo — el snapshot de una pose se regenera en cada
    // edición del armado en Posar y el cache por-path lo dejaba viejo.
    // evict_pack_textures tira TODO; esto es quirúrgico (espera GPU idle sólo
    // si la textura estaba cargada).
    void evict_sprite_texture(VkContext& ctx, const std::string& path);

    ///  fase 2: pre-calienta la textura de un asset SUELTO de disco (decode
    /// asincrono) — llamar al asignar/alimentar poses para que la primera
    /// aparicion no pague el decode (pico > frame budget → crackle de audio).
    /// flip = variante volteada (bit0 h, bit1 v). No-op si el path no existe.
    void prewarm_sprite(const std::string& path, uint8_t flip = 0);

    /// Vestuario: pre-calienta la MÁSCARA de tinte de una pose (decode R8 con
    /// clave propia `path#m…` — el mismo PNG puede estar asignado como asset y
    /// como máscara). Misma política que prewarm_sprite.
    void prewarm_sprite_mask(const std::string& path, uint8_t flip = 0);

    /// HOT-RELOAD de autoría (2026-07-24): stat de los assets HD cargados
    /// DESDE DISCO y evict de los que cambiaron/aparecieron — el próximo frame
    /// los recarga (imágenes editadas en graphics/ fuera del Lab se veían
    /// viejas hasta reiniciar). Llamar a baja cadencia (~1×/s).
    void poll_disk_sprite_textures(VkContext& ctx);

    // Destroy all resources. Safe on a partially-initialized renderer.
    void shutdown(VkContext& ctx);

    bool is_ready() const { return target_.is_ready(); }

    // Region util del ultimo frame subido (el fb cambia de modo de video).
    uint32_t emu_frame_w() const { return emu_w_; }
    uint32_t emu_frame_h() const { return emu_h_; }

    /// Readback (export MP4): graba en `cmd` la copia del offscreen (que quedó
    /// SHADER_READ_ONLY tras render()) a un buffer host-visible — barrera a
    /// TRANSFER_SRC + vkCmdCopyImageToBuffer (BGRA tightly packed, extent() del
    /// target) + barrera de vuelta. `dst` debe tener extent().w*h*4 bytes.
    void copy_target_to_buffer(VkCommandBuffer cmd, VkBuffer dst);

    // ---- Export de frames a CPU (MP4) ---------------------------------------
    // Recursos de readback PROPIOS (cmd pool TRANSIENT + fence + buffer VMA
    // host-visible con mapeo persistente — VMA es privado del engine, por eso
    // viven acá y no en el lab). El export no se ata al present: cada
    // export_frame renderiza `fv` al offscreen (que debe estar YA
    // redimensionado a la resolución destino con resize()), copia al buffer,
    // submitea con fence y ESPERA. Un solo buffer reusado (8K ≈ 95 MB host).
    /// Crea los recursos para frames de extent() actual. false = sin recursos.
    bool readback_init(VkContext& ctx);
    /// Render + copia + submit + wait. Devuelve el BGRA mapeado (w*h*4 del
    /// extent() del target; válido hasta el próximo export_frame) o nullptr.
    /// `vdp_mask` = los mismos ojos de capa del render (bits AYTHER_LAYER_*:
    /// A=1 B=2 W=4 OBJ=8). El Snapshot del Lab deja elegir qué capas entran en
    /// la imagen; 0xFF = todas, que es lo que hacían el export MP4 y el
    /// snapshot antes de que el parámetro existiera.
    const uint8_t* export_frame(VkContext& ctx, const FrameView& fv,
                                AyArchive* pack, bool hd_on,
                                const AytherLayerStack* layers = nullptr,
                                uint8_t vdp_mask = 0xFF);
    void readback_shutdown(VkContext& ctx);

    // Record the HD render for `fv` into `cmd`, targeting the offscreen image.
    // Uploads the emu frame (always) then, when hd_on=true, overlays the resolved
    // HD tile substitutions and HD sprite pass. hd_on=false gives the raw emulator
    // look (M4 Original mode). `pack` provides HD asset bytes; null → emu only.
    //
    // R-4 (): las lanes se despachan desde la LISTA DE CAPAS — orden
    // explícito, visibilidad por capa, capas Custom insertables (no-op hasta
    // R-7). `layers = nullptr` usa el stack por defecto (== el orden cableado
    // histórico: los smokes y el export no cambian). Las capas VDP del stack
    // NO se dibujan acá: su visibilidad viaja por la máscara de sesión
    // (AytherLayerStack::vdp_mask, ruteada por el frontend).
    // R-5 parte 2b: `vdp_mask` = los ojos de WORKSPACE (Editar/Pintar, bits
    // AYTHER_LAYER_*: A=1 B=2 W=4 OBJ=8) — el ex canal 0x102, ahora un filtro
    // por elemento del compose (AND con la visibilidad del stack). En frames
    // de fallback al blit no aplica (el blit trae el frame completo).
    void render(VkContext& ctx, VkCommandBuffer cmd, const FrameView& fv,
                AyArchive* pack, bool hd_on = true,
                const AytherLayerStack* layers = nullptr,
                uint8_t vdp_mask = 0xFF);

    // R-8 (): modo UV checker — vista de diagnóstico de autoría. Todo
    // elemento SIN asset se pinta con un checker anclado al elemento, con el
    // par de colores de SU CAPA (rosa+negro = autorado pero no cargó); el resto se
    // atenúa (los HD dibujan normal encima). Solo afecta los pases de escena;
    // en frames de fallback al blit no aplica.
    void set_checker(bool on) noexcept { checker_ = on; }
    bool checker() const noexcept { return checker_; }

    // Capa ENFOCADA: la que se está autorando se compone a intensidad plena y
    // el resto se atenúa, para que el foco de trabajo se lea de un vistazo.
    // Índice de capa de escena (0=Plano B · 1=Plano A · 2=Window · 3=Sprites);
    // -1 = sin foco (todo a intensidad plena).
    //
    // Vive acá y no en el canal 0x108 del fork porque el dim es una decisión de
    // COMPOSICIÓN, y la composición es nuestra desde R-5 (): aquel canal es
    // booleano —atenúa «lo no-sprite»— y no sabe de una capa enfocada.
    void set_focus_layer(int layer) noexcept { focus_layer_ = layer; }
    int  focus_layer() const noexcept { return focus_layer_; }

    /// R-8: estado de carga del asset del SUB de un elemento (para el reporte
    /// de cobertura del Lab). NotRequested/Pending/Ready/Failed; elementos sin
    /// sub devuelven NotRequested.
    VkSprite::TexState sub_texture_state(const FrameView& fv,
                                         const SceneElement& e) const;

    // ---- The offscreen result — the frontend presents or samples it --------
    VkImage     framebuffer_image()   const { return target_.image();   }
    VkImageView framebuffer_view()    const { return target_.view();    }
    VkSampler   framebuffer_sampler() const { return target_.sampler(); }
    VkExtent2D  framebuffer_extent()  const { return target_.extent();  }

    // ---- : imagen de COMPARACIÓN (preview A/B) --------------------------
    // Un segundo offscreen que guarda una COPIA del frame ya renderizado, para
    // poder mostrar dos versiones del mismo cuadro a la vez (split Original |
    // AYTHER). Existe porque el renderer tiene un solo `target_`: sin una copia,
    // dibujar las dos versiones exigiría renderizar dos veces por frame.
    //
    // El uso previsto es EN PAUSA — el gesto de autoría es comparar UN cuadro:
    // se renderiza la versión original, se captura acá, se renderiza la HD y se
    // dibujan las dos. En vivo funcionaría igual, pero el costo pasa a ser por
    // frame y esa decisión no la toma el renderer.
    //
    // No se crea hasta que alguien la pide (capture_compare): un Lab que nunca
    // abre el A/B no paga la memoria de un segundo canvas, que a 8K no es poca.
    bool        compare_ready() const { return compare_.is_ready(); }
    /// : la IMAGEN, para el split del runtime — que compone con blits por
    /// región y no muestreando, así que necesita el handle y no la vista.
    VkImage     compare_image()   const { return compare_.image();   }
    VkImageView compare_view()    const { return compare_.view();    }
    VkSampler   compare_sampler() const { return compare_.sampler(); }
    VkExtent2D  compare_extent()  const { return compare_.extent();  }

    /// Copia el offscreen ACTUAL (tal como lo dejó render()) a la imagen de
    /// comparación. Crea/redimensiona la imagen si hace falta. Grabar antes de
    /// re-renderizar el mismo cuadro con la otra configuración.
    /// false = no se pudo crear la imagen (sin memoria) — el caller degrada a
    /// mostrar una sola versión.
    bool capture_compare(VkContext& ctx, VkCommandBuffer cmd);
    /// Libera la imagen de comparación (salir del A/B devuelve la memoria).
    void compare_release(VkContext& ctx);

    /// Lee la imagen de comparación a CPU — BGRA del extent del target, con los
    /// mismos recursos de readback que `export_frame` (requiere readback_init).
    /// Existe para el ORÁCULO del A/B: lo que hace que la comparación no mienta
    /// es que la copia contenga exactamente lo que el offscreen tenía al
    /// capturar, y eso sólo se puede afirmar leyéndola. nullptr si no hay copia
    /// o el submit falla.
    const uint8_t* readback_compare(VkContext& ctx);

    /// Captura + submit propio, para usar capture_compare fuera de un frame
    /// (oráculos). En el Lab la captura viaja en el cmd del frame en curso.
    bool capture_compare_now(VkContext& ctx);

private:
    // Genesis Mode 5 max framebuffer — the native canvas the tile grid maps onto.
    static constexpr uint32_t kEmuW = 320;
    static constexpr uint32_t kEmuH = 240;

    VkRenderTarget target_;       // offscreen HD frame
    VkRenderTarget compare_;      // : copia para el A/B (lazy, ver arriba)
    // ---- Readback del export MP4 (readback_init/export_frame) --------------
    // VmaAllocation forward-declarado como en vk_texture.h (VMA privado).
    VkCommandPool   rb_pool_  = VK_NULL_HANDLE;
    VkCommandBuffer rb_cmd_   = VK_NULL_HANDLE;
    VkFence         rb_fence_ = VK_NULL_HANDLE;
    VkBuffer        rb_buf_   = VK_NULL_HANDLE;
    VmaAllocation   rb_alloc_ = VK_NULL_HANDLE;
    void*           rb_map_   = nullptr;
    VkTexture      emu_tex_;
    uint32_t  emu_w_ = 0, emu_h_ = 0;   // dims del ultimo frame subido      // emulator software framebuffer → GPU
    TileTexCache   tile_cache_;   // HD tile textures from the pack
    VkSprite       sprite_;       // HD sprite overlay pass (into the offscreen)
    bool           sprite_ok_ = false;  // false if the SPIR-V shaders are absent
    // R-5 (): el pipeline indexado (VRAM+CRAM en GPU) con el que se compone
    // la escena desde el inventario cuando fv.scene está publicada — sin blit.
    VkIndexedPlane indexed_;
    bool           indexed_ok_ = false;
    bool           checker_    = false;   // R-8 (): modo UV checker
    int            focus_layer_ = -1;     // capa enfocada (-1 = ninguna)
};

}  // namespace ayther
