#pragma once
// ---------------------------------------------------------------------------
// vk_sprite.h — Alpha-blended HD sprite overlay pipeline.  Ayther v0.8.0
//
// Renders HD sprite textures (loaded from a .ay pack) over the emulator
// framebuffer with standard src-alpha blending.
//
// ## Integration into the frame loop
//
//   // swap must be in TRANSFER_DST_OPTIMAL on entry.
//   sprite_pipeline.draw(ctx, swap, subs, n_subs, pack, emu_w, emu_h);
//   // swap is back in TRANSFER_DST_OPTIMAL on return.
//   VkPresent::finalize(ctx, swap);     // unchanged
//
// ## Lifecycle
//
//   sprite_pipeline.init(ctx, swap)          // once after swapchain creation
//   sprite_pipeline.rebuild(ctx, swap)       // on SDL_EVENT_WINDOW_RESIZED
//   sprite_pipeline.shutdown(ctx)            // before vkDeviceWaitIdle / shutdown
//
// ## Render-pass layout path (one barrier pair per draw() call)
//
//   TRANSFER_DST  →  [explicit barrier]  →  COLOR_ATTACHMENT
//                →  [render pass, loadOp=LOAD, alpha-blend subpass]
//                →  [finalLayout auto-transition]  →  TRANSFER_DST
//
//   VkPresent::finalize() then transitions TRANSFER_DST → PRESENT_SRC_KHR
//   as usual — no callers need to be changed.
// ---------------------------------------------------------------------------
#include <vulkan/vulkan.h>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class VkContext;
class VkTexture;
struct AyArchive;
struct AytherSpriteSub;
namespace ayther { struct AnimHdFrame; }   // ayther_animation.h (C-S2)

// ---------------------------------------------------------------------------

class VkSprite {
public:
    VkSprite() = default;
    ~VkSprite();

    VkSprite(const VkSprite&) = delete;
    VkSprite& operator=(const VkSprite&) = delete;

    // ---- Lifecycle -----------------------------------------------------------

    /// Initialise the render pass, alpha-blend pipeline, sampler, descriptor
    /// pool, and a framebuffer over `target_view` (the renderer's offscreen
    /// image, format `fmt`, size w×h). Call once after the target exists.
    bool init(VkContext& ctx, VkFormat fmt, uint32_t w, uint32_t h,
              VkImageView target_view,
              const char* vert_spv_path, const char* frag_spv_path);

    /// Recreate the framebuffer after the offscreen target is resized.
    /// The pipeline and render pass are format-fixed — they survive resizes.
    void rebuild(VkContext& ctx, uint32_t w, uint32_t h, VkImageView target_view);

/// Deterministic early release. The destructor performs the same idempotent
/// cleanup when the sprite owner leaves scope.
    void shutdown(VkContext& ctx);

    // ---- Per-frame draw ------------------------------------------------------

    /// Render alpha-blended HD sprites over `target_image` (the offscreen).
    ///
    /// Entry state: target_image in TRANSFER_DST_OPTIMAL.
    /// Exit  state: target_image in TRANSFER_DST_OPTIMAL.
    ///
    /// cmd     — the recording command buffer (the renderer's per-frame cmd).
    /// subs    — resolved sprite substitutions from SpriteSubstitutor.
    /// pack    — open .ay archive (null → no-op).
    /// emu_w/h — native emulator resolution (320×240) for pixel scale factor.
    /// flips   — opcional, paralelo a `subs` (length=count): bit0 hflip, bit1 vflip.
    ///           Para los tiles de plano (Fase 2c): una instancia volteada del patrón
    ///           usa la MISMA textura volteada al cargar (pre-flip, sin shader).
    ///           nullptr = sin flips (sprites reales).
    /// slots   — C8 (opcional, paralelo a `subs`): slot SAT por sub. Cuando se pasa,
    ///           los subs se dibujan por slot DESCENDENTE (frontmost = slot menor =
    ///           dibujado último = encima) para respetar la oclusión sprite-vs-sprite
    ///           entre HD superpuestos. nullptr = orden por asset_path (batching; para
    ///           tiles de plano, que no se solapan por SAT).
    void draw(VkContext& ctx, VkCommandBuffer cmd, VkImage target_image,
              const AytherSpriteSub* subs, uint32_t count,
              AyArchive* pack,
              uint32_t emu_w, uint32_t emu_h,
              const uint8_t* flips = nullptr,
              const uint8_t* tint  = nullptr,    ///< E1 cromático: tinte RGB por sub
                                                 ///< (stride 3, Q2.6: 64 = 1.0)
              const uint8_t* slots = nullptr,    ///< C8: slot SAT por sub (z-order)
              /// Atenuado de capa POR SUB (, opcional, paralelo a `subs`):
              /// opacidad 0-255 (255 = opaco). Sólo lo usan los tiles de plano,
              /// donde una misma llamada mezcla subs de capas distintas y el
              /// `set_dim` global no alcanza. nullptr = la opacidad de set_dim.
              const uint8_t* alphas = nullptr,
              ///  (Acetatos): modo de mezcla de la LLAMADA entera — 0 =
              /// alpha normal (pipeline de siempre) · 1 = aditivo (src·a + dst,
              /// el destello no queda lechoso). Por llamada y no por sub: la
              /// unidad es la capa, y el blend es estado fijo del pipeline.
              uint8_t blend = 0);

    /// Animaciones C-S2: dibuja frames HD de un SHEET en fase — igual que
    /// draw() pero cada quad muestrea el SUB-RECT (UV) `src_*` del sheet y el
    /// destino es un rect FLOAT (sub-píxel, para el tween geométrico Nivel 1).
    /// Mismo pipeline (el sub-rect viaja en push constants); mismos estados de
    /// entrada/salida que draw().
    void draw_anim(VkContext& ctx, VkCommandBuffer cmd, VkImage target_image,
                   const ayther::AnimHdFrame* frames, uint32_t count,
                   AyArchive* pack,
                   uint32_t emu_w, uint32_t emu_h);


    /// El frame vigente del VIDEO de la Cinemática (), a pantalla completa
    /// y OPACO (cubre el 100%: reemplaza la pantalla, no la compone).
    ///
    /// Tiene textura y descriptor PROPIOS, y eso no es duplicación gratuita:
    ///  · No puede ir por `draw()` — ésa resuelve la textura por `asset_path`
    ///    contra el cache, donde la entrada cae en la liberación diferida del
    ///    staging () y a los pocos frames deja de aceptar uploads EN
    ///    SILENCIO. El video quedaría congelado en su primer frame.
    ///  · Tiene recursos PROPIOS a propósito. El compose tenía los suyos y
    ///    compartirlos con un video de otras dimensiones habría disparado el
    ///    `shutdown+delete` dos veces por frame sobre una textura referenciada
    ///    por command buffers en vuelo. El compose se fue en ; el criterio
    ///    vale igual para el próximo que quiera reusarlos.
    ///
    /// `seq` es el contador de contenido de la sesión: si no cambió desde el
    /// último dibujo, se saltea la re-subida. Sin eso se paga el memcpy entero
    /// por cada frame de interfaz estando el video en pausa.
    ///
    /// Muestreo LINEAR (no NEAREST como el overlay): el canvas es la altura del
    /// display × 4/3 —en un display 4K son 2880×2160— así que el video casi
    /// siempre se AMPLÍA, y con NEAREST saldría a bloques. Es la primera
    /// textura del motor que no es ni pixel-art ni un máster HD.
    /// : recibe los TRES PLANOS del decoder (I420), no un BGRA. La
    /// conversión la hace video.frag. `y_stride` y compañía NO son `w`: libvpx
    /// alinea las filas.
    void draw_video(VkContext& ctx, VkCommandBuffer cmd, VkImage target_image,
                    const uint8_t* y, uint32_t y_stride,
                    const uint8_t* u, uint32_t u_stride,
                    const uint8_t* v, uint32_t v_stride,
                    uint32_t w, uint32_t h, uint64_t seq);

    bool is_ready() const { return pipeline_ != VK_NULL_HANDLE; }

    /// Free all cached sprite textures and reset the descriptor pool,
    /// without destroying the pipeline or framebuffers.
    ///
    /// Call this during a pack hotreload (after vkDeviceWaitIdle) to evict
    /// all loaded textures so they are re-fetched from the new pack.
    /// The pipeline stays alive — no need to re-call init().
    void clear_textures(VkContext& ctx);

    /// Evict UN asset (`path` y sus variantes volteadas `path#N`) del cache para
    /// que el próximo draw lo recargue del pack/disco. Para assets REESCRITOS en
    /// vivo (el snapshot de una pose se regenera en cada edición del armado en
    /// Posar) — sin esto el sub seguiría dibujando la textura vieja estirada al
    /// bbox nuevo. Espera GPU idle sólo si había una textura cargada.
    void evict(VkContext& ctx, const std::string& path);

    ///  fase 2: PRE-CALIENTA un asset — lee los bytes y encola el decode en
    /// el worker, sin bloquear. Llamar al asignar/alimentar poses: cuando el
    /// asset aparezca en pantalla la textura ya está decodificada (el upload lo
    /// hace pump_uploads) y no hay pico de decode en el hilo de render. Solo
    /// assets SUELTOS de disco (pack == nullptr y el path no existe → no-op,
    /// para no negative-cachear nombres relativos de pack).
    /// `mask` = Vestuario: la MISMA ruta puede estar asignada como asset y
    /// como máscara — la máscara se cachea con clave propia (`path#m…`) y se
    /// decodifica R8 (1 canal).
    void prewarm(const std::string& path, AyArchive* pack, uint8_t flip = 0,
                 bool mask = false);

    /// HOT-RELOAD de autoría (2026-07-24): stat de los assets cargados DESDE
    /// DISCO y evict de los que cambiaron de mtime — o que APARECIERON (el
    /// negative-cache de un PNG que no existía se levanta al crearse el
    /// archivo). El próximo draw los recarga. Llamar a baja cadencia (~1×/s):
    /// sin cambios es solo un stat por asset; con cambios paga el evict (GPU
    /// idle una vez). Los assets del PACK no se vigilan (pack inmutable hasta
    /// su propio hotreload).
    void poll_disk(VkContext& ctx);

    ///  fase 2: sube al GPU los decodes terminados (máx `max_uploads` por
    /// frame, acota el hitch). Llamar UNA vez por frame con el cmd en grabación,
    /// FUERA de un render pass — aunque no haya subs este frame.
    void pump_uploads(VkContext& ctx, VkCommandBuffer cmd, int max_uploads = 2);

    /// R-8 (): estado de la textura de un asset SIN dispararle la carga.
    /// La clave replica la de draw() (path + sufijo de flip). Solo lectura —
    /// para que el modo checker distinga «autorado pero el asset NO cargó»
    /// (negative-cache) de un asset sano o todavía en vuelo.
    enum class TexState { NotRequested, Pending, Ready, Failed };
    TexState texture_state(const std::string& path, uint8_t flip = 0) const;

    /// ATENUADO de los quads HD (): multiplica el tinte (`d`) y la opacidad
    /// (`a`) de `draw`/`draw_anim` hasta el próximo cambio. Lo mueve el
    /// compositor capa por capa — el dim de la capa enfocada tiene que alcanzar
    /// al HD, que es justamente lo que se está autorando. 1.0, 1.0 = sin atenuar.
    /// La opacidad va aparte del tinte porque oscurecer solo NO deja ver a
    /// través: el HD atenuado tapaba igual a la capa enfocada que tiene detrás.
    ///  fase 0: desplazamiento X en píxeles del ESPACIO EMU para el
    /// centrado del ensanchado. Es ESTADO y no parámetro por el mismo criterio
    /// que `set_dim`: los subs vienen en coordenadas nativas (0..320) y el
    /// canvas puede ser más ancho, así que el corrimiento es del PASE entero y
    /// no de cada quad. 0 (el default) no cambia un píxel.
    void set_wide_offset(float off_x_emu) { wide_off_x_ = off_x_emu; }

    void set_dim(float d, float a = 1.0f) noexcept { dim_ = d; dim_a_ = a; }

private:
    VkContext* context_ = nullptr;
    // ---- Vulkan objects ----
    VkRenderPass          render_pass_  = VK_NULL_HANDLE;
    VkDescriptorSetLayout desc_layout_  = VK_NULL_HANDLE;
    VkPipelineLayout      pipe_layout_  = VK_NULL_HANDLE;
    float                 wide_off_x_   = 0.0f;   ///<  fase 0 (set_wide_offset)
    float                 dim_          = 1.0f;   ///< atenuado de capa: tinte (set_dim)
    float                 dim_a_        = 1.0f;   ///< atenuado de capa: opacidad (set_dim)
    VkPipeline            pipeline_     = VK_NULL_HANDLE;
    /// : variante ADITIVA del pipeline de sprites (src·a + dst, mismo
    /// layout y mismos shaders — sólo cambia el estado de blend). Los Acetatos
    /// la eligen con el `blend` de draw(). NULL = init falló su creación (el
    /// draw aditivo cae al alpha normal, con motivo en el log).
    VkPipeline            pipeline_add_ = VK_NULL_HANDLE;
    /// /: variantes MULTIPLICAR y PANTALLA del pipeline de sprites —
    /// mismo layout y mismo vert, frag PROPIO (sprite_mult/sprite_screen: el
    /// mix hacia blanco/negro por la fuerza no se expresa con factores fijos
    /// sin premultiplicar) + factores de COLOR fijos:
    ///   mult   = (DST_COLOR, ZERO)             → out = dst · mix(1, c·tint, a·op)
    ///   screen = (ONE_MINUS_DST_COLOR, ONE)    → out = dst + (c·tint·a·op)·(1−dst)
    /// Los Acetatos las eligen con blend=2/3. NULL = su .spv faltó en init —
    /// ese modo cae al alpha normal, con motivo en el log (misma degradación
    /// que el video y el Vestuario).
    VkPipeline            pipeline_mult_   = VK_NULL_HANDLE;
    VkPipeline            pipeline_screen_ = VK_NULL_HANDLE;
    /// : sustractivo, oscurecer y aclarar — REUSAN los frags de arriba,
    /// solo cambia el VkBlendOp (y por eso no hay .spv nuevos que stagear):
    ///   sub (4) = frag screen + (ONE, ONE, REVERSE_SUBTRACT) → dst − c·f
    ///   min (5) = frag mult   + op MIN → min(dst, mix(1, c·tint, f))
    ///   max (6) = frag screen + op MAX → max(dst, c·tint·f)
    VkPipeline            pipeline_sub_ = VK_NULL_HANDLE;
    VkPipeline            pipeline_min_ = VK_NULL_HANDLE;
    VkPipeline            pipeline_max_ = VK_NULL_HANDLE;
    VkSampler             sampler_      = VK_NULL_HANDLE;
    /// CADENA de pools de descriptores (crece bajo demanda): el pool único de
    /// 128 assets ×2 sets se agotaba en proyectos reales (Golden Axe: 65 assets
    /// ×2 caras = 260 sets) y las texturas que perdían el sorteo del orden de
    /// carga quedaban negative-cacheadas TODA la sesión — poses invisibles en
    /// sus ventanas de frames (reporte Tyris f515-536/f559-568, 2026-07-16).
    std::vector<VkDescriptorPool> desc_pools_;

    // ---- Compose de primer plano (Editar) — buffer BGRA full-screen, NEAREST ----
    VkSampler        sampler_nn_   = VK_NULL_HANDLE;   // point sampler (mask nítido)

    // VESTUARIO — pipeline de 2 samplers (binding 0 = asset · 1 = máscara R8)
    // con sprite_mask.frag: sólo lo usan los quads CON máscara; el pipeline
    // clásico y sprite.frag quedan intactos (oráculos GPU byte-exactos). NULL =
    // el .spv no estaba en init — los quads con máscara caen al pipeline
    // clásico (tinte entero), con motivo en el log.
    VkDescriptorSetLayout mask_layout_      = VK_NULL_HANDLE;
    VkPipelineLayout      mask_pipe_layout_ = VK_NULL_HANDLE;
    VkPipeline            mask_pipeline_    = VK_NULL_HANDLE;
    /// Pools PROPIOS de los sets de 2 samplers (la cadena `desc_pools_` aloca
    /// con `desc_layout_`, que es de 1). Crece bajo demanda como aquélla.
    std::vector<VkDescriptorPool> mask_pools_;
    /// Set combinado (asset + máscara) por par de texturas y filtro. Guarda los
    /// seq de AMBAS entradas: un evict/hot-reload cambia el seq y el set se
    /// re-escribe antes del próximo bind (evict ya esperó GPU idle).
    struct MaskSet {
        VkDescriptorSet ds        = VK_NULL_HANDLE;
        uint32_t        asset_seq = 0;
        uint32_t        mask_seq  = 0;
    };
    std::unordered_map<std::string, MaskSet> mask_set_cache_;

    // VIDEO () — recursos PROPIOS, ver draw_video. Pool aparte por la misma
    // razón que el del overlay: clear_textures resetea `pool_` en el hotreload
    // del pack y se llevaría puesto el set del video a mitad de reproducción.
    VkDescriptorPool video_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet  video_ds_   = VK_NULL_HANDLE;
    /// : tres texturas R8 (luma + los dos cromas a la mitad) en vez de una
    /// BGRA8. Se suben 1,5 bytes por píxel en lugar de 4.
    std::unique_ptr<VkTexture> video_tex_[3];
    /// Pipeline propio del video: mismo render pass y mismo push constant que el
    /// de sprites, distinto fragment shader y tres bindings.
    VkDescriptorSetLayout video_layout_      = VK_NULL_HANDLE;
    VkPipelineLayout      video_pipe_layout_ = VK_NULL_HANDLE;
    VkPipeline            video_pipeline_    = VK_NULL_HANDLE;
    uint32_t         video_w_ = 0, video_h_ = 0;
    uint64_t         video_seq_ = 0;   // último `seq` subido (0 = nada todavía)

    std::vector<VkFramebuffer> framebuffers_;
    uint32_t                   fb_w_ = 0, fb_h_ = 0;

    // ---- Sprite texture cache ----
    // Dos descriptor sets por textura: NEAREST (pixel-art ampliado = nítido) y
    // LINEAR (arte HD MINIFICADO — un máster 1296² dibujado a ~72px con NEAREST
    // se ve pixelado). El draw elige por-quad según la dirección del escalado.
    struct TexEntry {
        std::unique_ptr<VkTexture> tex;
        VkDescriptorSet desc_set     = VK_NULL_HANDLE;   // sampler NEAREST
        VkDescriptorSet desc_set_lin = VK_NULL_HANDLE;   // sampler LINEAR (minificación)
        bool            valid    = false;
        bool            stale    = false;    // evict(): recargar en el próximo ensure_loaded
        bool            pending  = false;    // : decode asíncrono en vuelo
        uint32_t        seq      = 0;        // : invalida resultados viejos (evict)
        /// Hot-reload: mtime del archivo de DISCO al leer sus bytes (ticks de
        /// fs::file_time_type). -1 = vino del pack / no vigilar; -2 = el
        /// archivo NO existía al cargar (vigilar su aparición).
        int64_t         disk_mtime = -1;
    };
    std::unordered_map<std::string, TexEntry> tex_cache_;

    // ---- Liberación diferida del staging () ----------------------------
    // Las texturas de asset suben UNA vez pero VkTexture retiene su staging
    // host-visible toda la vida (hasta ~10 MB por máster → decenas de MB por
    // sesión). No se puede liberar en finish_upload: la copia recién quedó
    // GRABADA en el cmd de este frame. Se libera kStagingLingerPumps pumps
    // después (> frames-in-flight ⇒ el fence del frame del upload ya señalizó).
    // La entrada guarda key+seq: si hubo evict/reload el seq no coincide y se
    // salta (la textura NUEVA aún necesita su staging).
    struct StagingRelease { std::string key; uint32_t seq = 0; uint64_t due = 0; };
    static constexpr uint64_t kStagingLingerPumps = 4;   // frames-in-flight (2) + margen
    std::vector<StagingRelease> staging_release_;
    uint64_t                    pump_tick_ = 0;
    size_t                      staging_freed_total_ = 0;   // observabilidad (log)
    /// Costo medido del upload completo (): el presupuesto de 1 por frame
    /// se fijó suponiendo 4-5 ms para un master grande, y eso hay que
    /// verificarlo con el camino real antes de decidir si un frame de video por
    /// frame de juego entra. Sólo observabilidad — no cambia comportamiento.
    /// Desglose del costo (): init de imagen+mips · subida de píxeles ·
    /// descriptores. Acumulados; el log imprime la partición para saber cuál
    /// de las tres optimizaciones del issue vale la pena antes de escribirla.
    double                      up_init_ms_ = 0.0;
    double                      up_copy_ms_ = 0.0;
    double                      up_desc_ms_ = 0.0;
    /// Desglose fino de init, copiado de VkTexture::last_init_cost() por asset.
    /// Se acumula ACÁ y no en el .cpp de la textura porque TileTexCache también
    /// llama a init(): unos acumuladores compartidos divididos por el contador
    /// de assets hacían que las partes sumaran más que el todo.
    double                      up_img_ms_  = 0.0;
    double                      up_view_ms_ = 0.0;
    double                      up_stg_ms_  = 0.0;
    double                      up_log_ms_  = 0.0;
    double                      upload_ms_max_   = 0.0;
    double                      upload_ms_total_ = 0.0;
    double                      upload_mb_total_ = 0.0;
    int                         upload_count_    = 0;

    // ---- Decode asíncrono ( fase 2) ------------------------------------
    // stbi de un máster 1440×1296 son DECENAS de ms: hecho en el hilo de render
    // era un pico > presupuesto de frame en la PRIMERA aparición de cada asset
    // (y de cada variante volteada) → el backlog de audio (~46 ms) se vaciaba →
    // crackle ("degradación de sonido con HD",  fase 2). El worker hace
    // stbi + swap BGRA + pre-flip; el hilo de render solo lee bytes, encola y
    // sube (pump_uploads, acotado). Single worker: FIFO; los estados de
    // TexEntry se tocan SOLO en el hilo de render.
    struct DecodeJob  { std::string key; std::string path; uint8_t flip = 0;
                        bool r8 = false;   ///< Vestuario: decode a 1 canal (máscara)
                        uint32_t gen = 0, seq = 0; std::vector<uint8_t> raw; };
    struct DecodeDone { std::string key; std::string path;
                        bool r8 = false;
                        uint32_t gen = 0, seq = 0;
                        int w = 0, h = 0; std::vector<uint8_t> bgra; };
    std::thread             decode_worker_;
    std::mutex              decode_mx_;
    std::condition_variable decode_cv_;
    std::deque<DecodeJob>   decode_jobs_;
    std::vector<DecodeDone> decode_done_;
    bool                    decode_quit_ = false;
    uint32_t                decode_gen_  = 0;   // clear_textures() invalida lo en vuelo

    void decode_loop();
    /// Lee los bytes (pack/disco — acá, para no tocar AyArchive desde el worker)
    /// y encola el decode. Marca la entrada pending.
    void enqueue_decode(const std::string& key, const std::string& path,
                        AyArchive* pack, uint8_t flip, bool mask_r8 = false);
    /// Textura + descriptores desde BGRA ya decodificado (la mitad GPU del viejo
    /// ensure_loaded síncrono). true = entry.valid.
    /// `r8` = Vestuario: la textura sube R8/Gray8 y NO aloca los descriptor
    /// sets de 1 sampler (la máscara nunca se dibuja sola — vive en los sets
    /// combinados de `mask_set_cache_`).
    bool finish_upload(VkContext& ctx, VkCommandBuffer cmd, TexEntry& entry,
                       const std::string& path, int w, int h, const uint8_t* bgra,
                       bool r8 = false);

    // ---- Helpers ----
    bool create_render_pass  (VkContext& ctx, VkFormat fmt);
    /// : parametrizado por cantidad de SAMPLERS y con salida explícita,
    /// porque ahora hay dos pipelines: el de sprites (1 sampler, sprite.frag) y
    /// el del video (3 — luma y los dos cromas —, video.frag). Todo lo demás
    /// —render pass, blending, push constant, estado dinámico— es idéntico, así
    /// que duplicar la función habría sido duplicar 150 líneas para cambiar un
    /// entero y una ruta.
    /// : `out_pipeline_add` (opcional) crea ADEMÁS la variante aditiva del
    /// mismo pipeline (mismo layout/shaders, blend src·a + dst) — para los
    /// Acetatos. nullptr = sólo el pipeline alpha (video, compose).
    /// /: `variants` (opcional) crea variantes con FRAG PROPIO y
    /// factores de blend de COLOR fijos (multiplicar, pantalla), reusando
    /// layout/vert/estado. La degradación es POR variante: si su .spv falta o
    /// la creación falla, su `out` queda NULL (el draw cae al alpha normal) y
    /// create_pipeline NO fracasa por eso.
    /// : `color_op` completa la expresividad — REVERSE_SUBTRACT (restar),
    /// MIN (oscurecer) y MAX (aclarar) salen del MISMO par de frags de
    /// / cambiando solo el op (MIN/MAX ignoran los factores).
    struct PipelineBlendVariant {
        const char*   frag_spv;    ///< .frag.spv propio de la variante
        VkBlendFactor src_color;
        VkBlendFactor dst_color;
        VkBlendOp     color_op;
        VkPipeline*   out;
    };
    bool create_pipeline     (VkContext& ctx,
                              const char* vert_spv_path,
                              const char* frag_spv_path,
                              uint32_t    sampler_count,
                              VkDescriptorSetLayout* out_layout,
                              VkPipelineLayout*      out_pipe_layout,
                              VkPipeline*            out_pipeline,
                              VkPipeline*            out_pipeline_add = nullptr,
                              const PipelineBlendVariant* variants = nullptr,
                              uint32_t variant_count = 0);
    bool create_sampler      (VkContext& ctx);
    bool create_desc_pool    (VkContext& ctx);
    /// Aloca un set del ÚLTIMO pool de la cadena; si está agotado, crea un pool
    /// nuevo y reintenta. VK_NULL_HANDLE solo ante un fallo real del driver.
    VkDescriptorSet alloc_desc_set(VkContext& ctx);
    bool create_framebuffer  (VkContext& ctx, VkImageView view, uint32_t w, uint32_t h);
    void destroy_framebuffers(VkContext& ctx);

    /// Ensure the sprite texture for `path` is loaded into the GPU and its
    /// descriptor set is allocated.  Must be called OUTSIDE a render pass.
    /// `flip` (bit0 hflip, bit1 vflip) → variante volteada cacheada aparte (la
    /// textura se voltea al decodificar; la clave de caché lleva el sufijo de flip).
    TexEntry* ensure_loaded(VkContext& ctx, VkCommandBuffer cmd,
                            const std::string& path, AyArchive* pack,
                            uint8_t flip = 0, bool mask = false);

    /// Clave de cache de una textura: `path` · `path#N` (variante volteada) ·
    /// `path#m`/`path#mN` (Vestuario — el mismo PNG puede estar asignado como
    /// asset Y como máscara, y la máscara además se decodifica R8).
    static std::string tex_key(const std::string& path, uint8_t flip, bool mask) {
        if (!mask)
            return (flip & 3) ? path + "#" + static_cast<char>('0' + (flip & 3))
                              : path;
        std::string k = path + "#m";
        if (flip & 3) k += static_cast<char>('0' + (flip & 3));
        return k;
    }

    /// Set combinado asset+máscara (Vestuario): lo aloca de `mask_pools_` y lo
    /// (re)escribe cuando alguna de las dos entradas cambió de seq. nullptr =
    /// alguna textura no está lista todavía (el caller cae al pipeline clásico).
    VkDescriptorSet get_mask_set(VkContext& ctx, const std::string& asset_key,
                                 const std::string& mask_key, TexEntry& asset,
                                 TexEntry& mask, bool linear);
    VkDescriptorSet alloc_mask_set(VkContext& ctx);
    bool            create_mask_pool(VkContext& ctx);
};
