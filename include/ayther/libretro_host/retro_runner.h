#pragma once
#include <string>
#include <cstdint>
#include <functional>
#include <vector>
#include <map>
#include "core_loader.h"
#include "libretro.h"
#include "ayther_api.h"   // Versioned AYTHER core-extension contract.

// Owns a Libretro core, loads a game ROM, and drives the emulation loop.
// Phase 1 (v0.1.0): headless — logic + RAM access.
// Phase 2 (v0.2.0): video_cb_ feeds the TileHasher sprite fingerprinter.
// Phase 3 (v0.3.0): video_cb_ also drives the Vulkan renderer.
//
// With the versioned AYTHER extension available, reads use a captured frame
// snapshot and are validated against its generation. Controls use bounded,
// frame-aware operations and return explicit status codes. Without that
// extension, only supported standard or deprecated read paths are available;
// render and audio controls must report unsupported behavior instead of
// silently writing through raw memory pointers. The authoritative extension
// identifiers and layouts live in ayther_api.h and must not be duplicated here.

/// @brief Owns one libretro core instance and drives its frame callbacks.
///
/// Memory pointers returned by accessors are borrowed from the core and become
/// invalid when the core unloads or changes the corresponding allocation. The
/// runner is not thread-safe. Its C callback bridge uses process-visible
/// dispatch state, so multiple runners must not execute concurrently.
class RetroRunner {
public:
    RetroRunner();
    ~RetroRunner();

    RetroRunner(const RetroRunner&)            = delete;
    RetroRunner& operator=(const RetroRunner&) = delete;

    // Load the core DLL and the ROM file. Returns false on any error.
    bool init(const std::string& core_path, const std::string& rom_path);

    // Advance emulation by exactly one hardware tick.
    void run_frame();

    void shutdown();

    // --- RAM access (read-only, Phase 1) ---
    // Returns pointer to the 64 KB 68000 work RAM, or nullptr if not running.
    const uint8_t* work_ram()      const { return static_cast<const uint8_t*>(ram_ptr_); }
    size_t         work_ram_size() const { return ram_size_; }

    // Returns the core-owned mutable work-RAM view. Writes are immediately
    // visible to the emulated CPU. AytherSession owns the policy that marks the
    // session modified and prevents invalid recording assumptions.
    uint8_t* work_ram_mut() { return static_cast<uint8_t*>(ram_ptr_); }

    // --- VRAM access (read-only, v0.8.0 — SpriteHasher) ---
    // Returns pointer to the core's VRAM (RETRO_MEMORY_VIDEO_RAM = 3), or nullptr.
    // For Genesis Plus GX this is typically 64 KB.
    // NOTE: the pointer is owned by the core and remains valid until shutdown().
    [[deprecated("Use read_vram_v1() with the versioned AYTHER extension")]]
    const uint8_t* video_ram() const {
        if (!fn_retro_get_memory_data) return nullptr;
        return static_cast<const uint8_t*>(fn_retro_get_memory_data(3 /*RETRO_MEMORY_VIDEO_RAM*/));
    }
    size_t video_ram_size() const {
        if (!fn_retro_get_memory_size) return 0;
        return fn_retro_get_memory_size(3 /*RETRO_MEMORY_VIDEO_RAM*/);
    }

    // --- CRAM access (read-only — Mapa multi-espacio del Maper) -------------
    // Id PRIVADO del fork Ayther de GPX (no hay id libretro estándar para
    // CRAM): 128 bytes = 64 colores de 9 bits como uint16 host-endian en el
    // layout interno de GPX (0000BBB0GGG0RRR0). Cores stock devuelven null.
    static constexpr unsigned kAytherMemoryCram = 0x100;
    [[deprecated("E-5: usar read_cram_v1() con ABI v1")]]
    const uint8_t* color_ram() const {
        if (!fn_retro_get_memory_data) return nullptr;
        return static_cast<const uint8_t*>(
            fn_retro_get_memory_data(kAytherMemoryCram));
    }
    size_t color_ram_size() const {
        if (!fn_retro_get_memory_size) return 0;
        return fn_retro_get_memory_size(kAytherMemoryCram);
    }

    // --- VDP registers (read-only — Maper tilemap viewer, M9.3) -------------
    // Id PRIVADO del fork: los 32 registros del VDP (reg[0x20]). El Lab deriva
    // las bases de las name tables de planos y el tamaño del plano.
    static constexpr unsigned kAytherMemoryVdpRegs = 0x101;
    [[deprecated("E-5: usar read_vdp_regs_v1() con ABI v1")]]
    const uint8_t* vdp_regs() const {
        if (!fn_retro_get_memory_data) return nullptr;
        return static_cast<const uint8_t*>(
            fn_retro_get_memory_data(kAytherMemoryVdpRegs));
    }
    size_t vdp_regs_size() const {
        if (!fn_retro_get_memory_size) return 0;
        return fn_retro_get_memory_size(kAytherMemoryVdpRegs);
    }

    // --- VSRAM (read — scroll vertical) -------------------------------------
    // 128 bytes = 64 entradas u16 (11 bits de vscroll). El hscroll vive en VRAM;
    // esto es lo único que faltaba para resolver la posición en pantalla de un
    // tile de plano (Fase 2c). Cores stock devuelven null.
    static constexpr unsigned kAytherMemoryVsram = 0x107;
    [[deprecated("E-5: usar read_vsram_v1() con ABI v1")]]
    const uint8_t* vsram() const {
        if (!fn_retro_get_memory_data) return nullptr;
        return static_cast<const uint8_t*>(
            fn_retro_get_memory_data(kAytherMemoryVsram));
    }
    size_t vsram_size() const {
        if (!fn_retro_get_memory_size) return 0;
        return fn_retro_get_memory_size(kAytherMemoryVsram);
    }

    // --- RAM del Z80 (, ABI 1.9) -----------------------------------------
    //
    // Los 8 KB que el 68k ve en 0xA00000-0xA01FFF. El 68k y el Z80 se reparten
    // el trabajo de sonido, y varios juegos dejan ahí el id del tema a tocar —
    // no en work RAM.
    //
    // POR QUÉ HAY UN ACCESSOR PARA ESTO. Para grabar un tema limpio desde el
    // Sound Test hay que encontrar la casilla donde el 68k deja el id. En
    // Golden Axe el diferencial de work RAM dejó dos candidatos y la
    // confirmación automática los descartó a los dos: la única hipótesis que
    // quedaba era esta RAM, y no había dónde mirar.
    //
    // Cores stock devuelven null, como con el resto de las regiones del fork.
    static constexpr unsigned kAytherMemoryZ80Ram = 0x10F;
    const uint8_t* z80_ram() const {
        if (!fn_retro_get_memory_data) return nullptr;
        return static_cast<const uint8_t*>(
            fn_retro_get_memory_data(kAytherMemoryZ80Ram));
    }
    /// La misma región, mutable. ESCRIBIRLA MIENTRAS EL Z80 CORRE ES UNA
    /// CARRERA: hay que hacerlo con el bus tomado, o aceptar que lo escrito
    /// puede durar un frame — que para disparar un sonido por id alcanza, y
    /// para cualquier otra cosa no.
    uint8_t* z80_ram_mut() {
        if (!fn_retro_get_memory_data) return nullptr;
        return static_cast<uint8_t*>(
            fn_retro_get_memory_data(kAytherMemoryZ80Ram));
    }
    size_t z80_ram_size() const {
        if (!fn_retro_get_memory_size) return 0;
        return fn_retro_get_memory_size(kAytherMemoryZ80Ram);
    }

    // --- Parsed sprites this frame (READ list 0x10B + WRITE-reset count 0x10C) ---
    // Detección de sprites por lo que parse_satb REALMENTE parseó: entradas de
    // **10 bytes** — yr/xr/attr u16 + w/h u8 + sat_idx/chain_pos u8 (el
    // `ayther_sprite_v1` del fork), deduplicado. El consumidor real
    // (`ayther_sprite_hasher_process_sprites`) usa ese stride; este comentario
    // decía 8 y hacía leer corrido a quien le creyera. Es robusto a que el juego reescriba el
    // SAT a mitad de frame / cambie su base (genio del logo Sega de Aladdin), donde
    // leer el SAT a fin de frame muestra solo placeholders. Reset (count=0) antes de
    // run_frame; leer después. No-op con core stock → la FFI cae al autodetect.
    // --- Export arbitrario del core (sondas/spikes — ) -------------------
    // Resuelve un símbolo exportado del módulo REALMENTE cargado por ESTE
    // runner (con multi-instancia cada runner carga SU copia del DLL, con
    // estáticos propios: resolver sobre otra carga sería tocar OTRO estado).
    // nullptr con un core que no lo exporte (stock → degradación limpia).
    template <typename FnPtr>
    FnPtr core_sym(const char* name) const { return loader_.sym<FnPtr>(name); }

    // --- ABI AYTHER v1 (E-1, ) -------------------------------------------
    // El contrato VERSIONADO del fork, negociado al cargar el DLL. Hasta acá
    // todo el diálogo con el core iba por `retro_get_memory_data(0x100-0x10E)`:
    // punteros mutables crudos, sin versión, sin validación y sin forma de
    // preguntar qué entiende el core que hay del otro lado.
    //
    // La negociación es OPCIONAL y no participa del éxito de la carga: un core
    // stock no exporta el símbolo y eso es perfectamente válido — se sigue por
    // el camino legacy. `has_ayther_v1()` es el gate con el que los callers
    // eligen un camino u otro (E-3/E-4).

    /// True si el core exporta la ABI v1 y la negociación fue exitosa.
    bool has_ayther_v1() const { return ayther_api_ != nullptr; }

    /// El medio cargado es una imagen de DISCO (.iso/.cue/.chd) — o sea Sega CD,
    /// el único hardware con chips que el router de voces no espeja: el PCM
    /// RF5C164 y el CDDA. Se resuelve por la extensión al init, antes del primer
    /// frame, y no por lo que el juego llegue a tocar ().
    bool cd_media() const { return cd_media_; }

    /// Descriptor negociado; nullptr si `!has_ayther_v1()`. Propiedad del core:
    /// vale hasta `shutdown()` y sólo se toca desde el hilo de emulación.
    const ayther_interface_v1* ayther_api() const { return ayther_api_; }

    /// La versión de la ABI que declara el core (0 sin ABI). Major/minor con
    /// `AYTHER_ABI_VERSION_MAJOR/MINOR`. La regla es major == 1 y minor >= lo
    /// que se necesite, nunca `==`: la ABI es aditiva (guía 1.9 §3).
    uint32_t ayther_abi_version() const { return ayther_api_ ? ayther_api_->abi_version : 0u; }

    /// Las suscripciones que este Engine CONSUME — y sólo ésas (ABI 1.9, guía
    /// §4: «pedir sólo lo que se va a leer»). Hasta la ABI 1.3 esto era
    /// `AYTHER_SUB_ALL` y daba lo mismo, porque los siete bits eran los siete
    /// que se leían. Desde 1.9 `AYTHER_SUB_ALL` es 0xFFF e incluye ATTRIBUTION
    /// (un byte por pixel), LINE_STATE / LINE_CRAM / LINE_CELLS y FRAME_HASH
    /// (~100 KB recorridos por frame): cada bit activo mueve el renderer del
    /// core al clon observado y paga lo que captura, y nadie en el Engine lee
    /// todavía esas regiones. Cuando aparezca el consumidor (anclaje por tile,
    /// FRAME_HASH del Lab) se agrega SU bit acá, junto con quien lo lee.
    static constexpr uint32_t kEngineSubscriptions =
        AYTHER_SUB_VDP_MEMORY | AYTHER_SUB_SPRITE_CAPTURE | AYTHER_SUB_RENDER_CONTROLS |
        AYTHER_SUB_RASTER_TRACKING | AYTHER_SUB_AUDIO_WRITES | AYTHER_SUB_RECOMPOSITION |
        AYTHER_SUB_AUDIO_EVENTS;

    /// Bits de `fallback_reasons` (snapshot) / 0x10E (legacy). El header 1.9 no
    /// los nombra; la guía de integración §5.8 sí, y de ahí salen. `> 0` sigue
    /// siendo «fallback» para todo el mundo; estos dos merecen distinguirse:
    /// OVERFLOW = el journal pasó de 256 eventos y `recompose_multilayer`
    /// devuelve RC_JOURNAL_OVERFLOW en vez de un prefijo plausible;
    /// UNSUPPORTED_CONTROLS = un control que pedimos no aplica en este modo.
    static constexpr uint32_t kRasterReasonJournalOverflow     = UINT32_C(1) << 7;
    static constexpr uint32_t kRasterReasonUnsupportedControls = UINT32_C(1) << 8;

    /// Pide lo que el Engine consume (`kEngineSubscriptions`), acotado a lo que
    /// el core soporta. Devuelve la máscara activada (0 con un core sin ABI,
    /// que es el camino legacy y no necesita pedir).
    ///
    /// El fork no instrumenta NADA hasta que alguien lo declara: sin esto el log
    /// de escrituras de chip viene vacío, el probe de audio no emite y la
    /// máscara de mute se ignora EN SILENCIO. Un consumidor que no lo sabe no ve
    /// un error: ve ceros, que es exactamente lo que un oráculo confunde con
    /// «esto está en silencio como esperaba» (2026-08-13: cuatro herramientas
    /// medían un core mudo y lo reportaban como resultado).
    ///
    /// Es idempotente y no pisa nada. Los tests de la ABI (`abi_*`) piden a mano
    /// a propósito — ahí la suscripción ES lo que se está probando.
    uint32_t subscribe_all_supported() {
        if (!ayther_api_ || !(ayther_api_->capabilities & AYTHER_CAP_SUBSCRIPTIONS_V1))
            return 0;
        ayther_subscription_state_v1 st{};
        st.struct_size = sizeof(st);
        if (ayther_api_->get_subscriptions(&st, sizeof(st)) != AYTHER_STATUS_OK)
            return 0;
        const uint32_t want = kEngineSubscriptions & st.supported_mask;
        return ayther_api_->set_subscriptions(want) == AYTHER_STATUS_OK ? want : 0;
    }


    // --- Lecturas por la ABI v1 (E-3, ) ----------------------------------
    // Paralelas a los accessors legacy, que siguen intactos: el caller elige con
    // `has_ayther_v1()`. La diferencia de fondo con el camino viejo es que acá
    // la lectura VALIDA — hay una generación de snapshot que dice si lo leído
    // corresponde al frame que se cree, y un estado de suscripción que distingue
    // «no hay datos» de «nadie los pidió». Con los punteros crudos las dos
    // situaciones se ven igual: memoria con algo adentro.
    struct AytherReadResult {
        int32_t  status     = AYTHER_STATUS_UNSUPPORTED;
        uint32_t count      = 0;   ///< elementos leídos
        uint64_t generation = 0;   ///< generación que devolvió el core
        bool ok() const { return status == AYTHER_STATUS_OK; }
    };

    /// Snapshot del frame ACTUAL. Llamar después de `run_frame()`: la ABI
    /// resetea sus contadores en el frame boundary, así que este snapshot es el
    /// que reemplaza a los `reset_*()` manuales del camino legacy.
    AytherReadResult capture_frame_snapshot(ayther_frame_snapshot_v1& out) const;

    /// `SYSTEM` (ABI 1.5): modo del VDP (4/5), h40, interlace, S/H, PAL y el
    /// viewport del frame emitido con su offset (Game Gear = 160×144 en
    /// (48,24)). Sin suscripción; se llena al leer. Es la fuente del modo en
    /// vez de decodificar registros: esa decodificación ya se corrigió una vez
    /// en el core y la copia del Engine no se enteró (guía §5.1). Devuelve
    /// UNSUPPORTED sin la capability.
    AytherReadResult read_system_v1(ayther_system_v1& out) const;

    /// Región entera al buffer del caller, validando la generación.
    AytherReadResult read_region_v1(uint32_t region, void* out, uint32_t bytes,
                                    uint64_t generation) const;

    // VDP (AYTHER_SUB_VDP_MEMORY). `out` debe tener el byte_size de la región.
    /// Cuántos bytes dice la ABI que mide una región (0 si no hay ABI o no la
    /// conoce). Los `read_*_v1` de VDP escriben ESE tamaño, no el que reporta
    /// `retro_get_memory_size`: quien dimensione el buffer con el número legacy
    /// está apostando a que las dos fuentes coincidan. Hoy coinciden; esto
    /// existe para que el caller no tenga que apostar.
    size_t abi_region_bytes(uint32_t region) const;

    AytherReadResult read_vram_v1    (void* out, const ayther_frame_snapshot_v1& s) const;
    AytherReadResult read_cram_v1    (void* out, const ayther_frame_snapshot_v1& s) const;
    AytherReadResult read_vdp_regs_v1(void* out, const ayther_frame_snapshot_v1& s) const;
    AytherReadResult read_vsram_v1   (void* out, const ayther_frame_snapshot_v1& s) const;

    /// Sprites parseados (AYTHER_SUB_SPRITE_CAPTURE), con FORWARD-COMPAT: si el
    /// core declara un `element_size` mayor que el struct que este Engine
    /// conoce, se lee a un temporal y se copian sólo los campos conocidos. Sin
    /// eso, un core más nuevo desalinearía la lectura entera — que es
    /// exactamente lo que venía pasando por coincidencia con el puntero legacy
    /// (8 bytes leídos de un layout que hoy tiene 10).
    AytherReadResult read_parsed_sprites_v1(ayther_sprite_v1* out, uint32_t max,
                                            const ayther_frame_snapshot_v1& s) const;

    /// Escrituras de chip del frame (AYTHER_SUB_AUDIO_WRITES).
    AytherReadResult read_audio_writes_v1(ayther_audio_write_v1* out, uint32_t max,
                                          const ayther_frame_snapshot_v1& s) const;

    /// Razones de fallback del raster (AYTHER_SUB_RASTER_TRACKING). El snapshot
    /// ya las trae; 0 si no hay ABI o no se está suscripto.
    uint32_t read_raster_fallback_v1(const ayther_frame_snapshot_v1& s) const;

    // --- Frame Delta Stream (E-6, ) --------------------------------------
    // Qué se ensució en el frame, dicho por el core en vez de deducido: un byte
    // por *pattern name* (32 bytes de VRAM cada uno) más los contadores del
    // frame, incluido `raster_event_count` — el tamaño del journal de eventos
    // raster, que es el único dato de acá que el snapshot NO trae y el insumo
    // del replay multicapa de .
    //
    // Se refresca solo, dentro de `run_frame()`. Ver `poll_frame_delta_()` para
    // por qué el poll vive ahí y no en el caller.
    //
    // LO QUE ESTO **NO** HABILITA, y conviene saberlo antes de intentarlo: no
    // sirve para dejar de leer VRAM entera en `refresh_abi_mirror()`. No por el
    // dato —desde  el bitmask es un superconjunto fiel de lo que cambió,
    // verificado frame a frame en `abi_frame_delta`— sino porque no hay nada que
    // ganar: leer los 64 KiB por la ABI mide **0,002 ms/frame**, el 0,01% de un
    // frame de 16,6 ms. La invalidación selectiva cambiaría una lectura lineal
    // por un recorrido de 2048 bytes y N llamadas a `read_region`, para ahorrar
    // dos microsegundos y agregar un estado incremental que hay que invalidar
    // bien en cada reset, unserialize y cambio de core.
    bool has_frame_delta() const { return last_delta_ok_; }
    /// Válido sólo con `has_frame_delta()`; queda del último `run_frame()`.
    const ayther_frame_delta_v1& frame_delta() const { return last_delta_; }

    // --- Eventos de audio tipificados () ---------------------------------
    // El SEGUNDO camino de audio, y existe por una razón concreta: el chip PCM
    // de Sega CD no tiene bus expuesto, así que `read_audio_writes_v1` —que
    // transporta escrituras crudas de FM y PSG— no lo lleva ni lo puede llevar.
    // Este camino trae eventos YA TIPIFICADOS (key-on/off, volumen, pitch).
    //
    // Quién manda para qué chip, por escrito: las escrituras crudas son la
    // fuente para FM y PSG; los eventos, para el PCM. La IDENTIDAD de un sonido
    // no la decide ninguno de los dos — la calcula el detector
    // (`core/src/audio_event.rs`) para los tres chips por igual.
    //
    // Es CONSUMO-AL-POLLEAR sobre una cola SPSC: lo que se lee desaparece, así
    // que hay un solo consumidor y llama una vez por frame. `event_size` sale
    // del core y no de un `sizeof` local — el struct pasó a ser una unión y
    // cambió de tamaño una vez ya.
    //
    // Devuelve la cantidad de eventos escritos en `out` (0 sin ABI, sin
    // suscripción o sin nada pendiente).
    uint32_t poll_audio_events_v1(ayther_audio_event_v1* out, uint32_t max) const;
    /// Eventos que el transporte descartó por falta de polleo, acumulado.
    uint32_t audio_events_dropped() const;

    // --- Escrituras de control por la ABI v1 (E-4, ) ---------------------
    // Las escrituras legacy son `*p = valor` sobre memoria del core: no validan
    // bounds, no avisan del cambio de generación (el sistema de snapshots queda
    // desincronizado) y nada impide hacerlas en pleno `retro_run`. `write_control`
    // valida las tres cosas y devuelve la generación nueva.
    struct AytherWriteResult {
        int32_t  status         = AYTHER_STATUS_UNSUPPORTED;
        uint64_t new_generation = 0;
        bool ok() const { return status == AYTHER_STATUS_OK; }
    };

    /// Escritura en una región de CONTROL, entre frames.
    /// `AYTHER_GENERATION_ANY` omite la validación de generación, que es lo
    /// correcto para un control que se fija fuera de un snapshot activo.
    AytherWriteResult write_control_v1(
        uint32_t region, const void* data, uint32_t bytes,
        uint64_t expected_generation = AYTHER_GENERATION_ANY) const;

    // ---- Controles de render y audio (AYTHER_SUB_RENDER_CONTROLS) ----------
    // Los tamaños son parte del CONTRATO de cada control, no un detalle del
    // caller: viven acá para que nadie los repita a mano.
    static constexpr uint32_t kSpriteSuppressBytes    = 16;        // 128 slots SAT
    static constexpr uint32_t kTileSuppressBytes      = 512;       // 64×64 celdas
    static constexpr uint32_t kPlaneTileSuppressBytes = 3 * 1024;  // 3 planos

    /// Máscara de capas visibles: 1 byte con los bits A/B/Window/Sprites. El
    /// renderer la lee POR LÍNEA, así que oculta o muestra capas en vivo.
    AytherWriteResult set_layer_mask_v1(uint8_t mask) const {
        return write_control_v1(AYTHER_REGION_LAYER_MASK, &mask, 1);
    }
    /// 0 = render normal (bit-exact). !=0 = los píxeles que NO son de sprite se
    /// emiten al 25%, para que los sprites preponderen (Lab Animación).
    AytherWriteResult set_layer_dim_v1(uint8_t on) const {
        return write_control_v1(AYTHER_REGION_LAYER_DIM, &on, 1);
    }
    /// Bitmask de 128 bits: slots de la SAT que `parse_satb` va a saltear.
    AytherWriteResult set_sprite_suppress_v1(const uint8_t* bits16) const {
        return write_control_v1(AYTHER_REGION_SPRITE_SUPPRESS,
                                bits16, kSpriteSuppressBytes);
    }
    /// Celdas de salida de 8px (64×64, stride 64) que se pintan con el backdrop
    /// y revelan el fondo del VDP. La aplica `render_line`.
    AytherWriteResult set_tile_suppress_v1(const uint8_t* bits, uint32_t n) const {
        return write_control_v1(AYTHER_REGION_TILE_SUPPRESS, bits, n);
    }
    /// 3 planos × bitmap de (patrón<<2 | paleta): `render_bg_m5/_vs` saltean esas
    /// celdas y revelan el plano de atrás. El fork tenía un flag `active` aparte
    /// (0x106) sin el cual esto era un no-op SILENCIOSO; por la ABI el core lo
    /// administra solo — medido en `abi_write_control` ().
    AytherWriteResult set_plane_tile_suppress_v1(const uint8_t* bits, uint32_t n) const {
        return write_control_v1(AYTHER_REGION_PLANE_TILE_SUPPRESS, bits, n);
    }
    /// Canales a silenciar. El canal se pone a 0 en el mixer de SALIDA sin tocar
    /// el estado del chip: replay-safe, el chip evoluciona idéntico y sólo
    /// cambia el PCM emitido — el hasher tiene que seguir viéndolo (). Es el
    /// primitivo de la sustitución por evento: mutear los canales de un evento
    /// mientras suena su asset HD. 0 = todo suena.
    static constexpr uint32_t audio_mute_fm(int ch)  { return uint32_t(1u << ch); }        // ch 0-5
    static constexpr uint32_t audio_mute_psg(int ch) { return uint32_t(1u << (6 + ch)); }  // ch 0-3
    static constexpr uint32_t audio_mute_pcm(int ch) { return uint32_t(1u << (10 + ch)); } // ch 0-7
    AytherWriteResult set_audio_mute_v1(uint32_t mask) const {
        return write_control_v1(AYTHER_REGION_AUDIO_MUTE, &mask, sizeof(mask));
    }

    // --- Señal de fidelidad por frame (R-5 , id 0x10E) -------------------
    // u32 ESCRIBIBLE: escrituras con efecto visual a mitad de pantalla
    // (CRAM/VSRAM/tabla de hscroll/regs). El frontend la resetea antes del
    // frame visible y la lee después; >0 = el frame NO se recompone fiel desde
    // el estado final (R-1) → ese frame cae al blit (híbrido). 0 con core stock.
    static constexpr unsigned kAytherMemoryRasterDirty = 0x10E;
    [[deprecated("E-5: usar read_raster_fallback_v1() con ABI v1")]]
    uint32_t raster_dirty() const {
        if (!fn_retro_get_memory_data) return 0;
        const auto* p = static_cast<const uint32_t*>(
            fn_retro_get_memory_data(kAytherMemoryRasterDirty));
        return p ? *p : 0;
    }

    static constexpr unsigned kAytherMemoryParsedSprites = 0x10B;
    static constexpr unsigned kAytherMemoryParsedCount   = 0x10C;
    [[deprecated("E-5: usar read_parsed_sprites_v1() con ABI v1")]]
    const uint8_t* parsed_sprites() const {
        if (!fn_retro_get_memory_data) return nullptr;
        return static_cast<const uint8_t*>(
            fn_retro_get_memory_data(kAytherMemoryParsedSprites));
    }
    [[deprecated("E-5: usar capture_frame_snapshot() con ABI v1")]]
    uint8_t parsed_sprite_count() const {
        if (!fn_retro_get_memory_data) return 0;
        const auto* p = static_cast<const uint8_t*>(
            fn_retro_get_memory_data(kAytherMemoryParsedCount));
        return p ? *p : 0;
    }

    // --- Audio chip writes this frame (READ log 0x109 + READ/WRITE-reset count 0x10A) ---
    // Log temporal de escrituras CRUDAS a los chips de sonido — YM2612 (FM) + SN76489
    // (PSG) — en orden de bus dentro del frame. Cada AudioWrite = {cycle, addr, data,
    // chip} (8 bytes, ABI idéntico al AytherAudioWrite del fork). Es la base de la
    // identidad de audio por SECUENCIA DE COMANDOS al chip (estable a través del
    // replay, porque la CPU/VDP son byte-deterministas) en lugar de hashear el PCM de
    // salida, que NO es reproducible tras unserialize (la fase del FM diverge). Mismo
    // patrón frontend-reset que parsed_sprites: reset (count=0) antes de run_frame,
    // leer después. No-op con core stock (degradación limpia → la FFI cae al PCM).
    struct AudioWrite { uint32_t cycle; uint16_t addr; uint8_t data; uint8_t chip; };
    static constexpr unsigned kAytherMemoryAudioWrites = 0x109;
    static constexpr unsigned kAytherMemoryAudioCount  = 0x10A;
    static constexpr uint8_t  kAudioChipFM  = 0;   // YM2612
    static constexpr uint8_t  kAudioChipPSG = 1;   // SN76489
    [[deprecated("E-5: usar read_audio_writes_v1() con ABI v1")]]
    const AudioWrite* audio_writes() const {
        if (!fn_retro_get_memory_data) return nullptr;
        return static_cast<const AudioWrite*>(
            fn_retro_get_memory_data(kAytherMemoryAudioWrites));
    }
    [[deprecated("E-5: usar capture_frame_snapshot() con ABI v1")]]
    uint32_t audio_write_count() const {
        if (!fn_retro_get_memory_data) return 0;
        const auto* p = static_cast<const uint32_t*>(
            fn_retro_get_memory_data(kAytherMemoryAudioCount));
        return p ? *p : 0;
    }

    // --- Cheats (modo avanzado del Lab: codigos GG/PAR via el core) --------
    void cheat_set(unsigned index, bool enabled, const char* code) {
        if (fn_retro_cheat_set) fn_retro_cheat_set(index, enabled, code);
    }
    void cheat_reset() {
        if (fn_retro_cheat_reset) fn_retro_cheat_reset();
    }

    bool is_running() const { return running_; }

    // Frames-per-second reported by the core via retro_system_av_info.timing.fps.
    // Available after init(). Returns 60.0 before the ROM is loaded.
    double fps() const { return fps_; }

    // Pixel format set by the core (RETRO_PIXEL_FORMAT_*).
    // 0 = 0RGB1555 (legacy), 1 = XRGB8888, 2 = RGB565 (Genesis Plus GX default).
    unsigned pixel_format() const { return pixel_format_; }

    // Optional video callback (wired to TileHasher in Phase 2, Vulkan in Phase 3).
    using VideoCb = std::function<void(const void*, unsigned, unsigned, size_t)>;
    void set_video_callback(VideoCb cb) { video_cb_ = std::move(cb); }

    // Optional audio callback wired to AudioHasher.
    // Signature mirrors retro_audio_sample_batch_t: returns frames consumed.
    using AudioCb = std::function<size_t(const int16_t*, size_t)>;
    void set_audio_callback(AudioCb cb) { audio_cb_ = std::move(cb); }

    // ----- Savestates (R2 base · cánones de emulador) -----------------------
    // Genesis Plus GX ≈ 150–250 KB/estado. Used by the determinism spike and,
    // later, by rewind/.arp recordings.

    /// Size in bytes of a serialized state, or 0 if unsupported.
    size_t serialize_size() const {
        return fn_retro_serialize_size ? fn_retro_serialize_size() : 0;
    }
    /// Capture the current state into `out`. Returns false on failure.
    /// (Multi-instancia : cada entry point re-aserta s_instance_ — con un
    /// shadow core activo los callbacks deben rutear a ESTA instancia.)
    bool serialize(std::vector<uint8_t>& out) const {
        const size_t n = serialize_size();
        if (!fn_retro_serialize || n == 0) return false;
        s_instance_ = const_cast<RetroRunner*>(this);
        out.resize(n);
        return fn_retro_serialize(out.data(), out.size());
    }
    /// Restore a state captured by serialize(). Returns false on failure.
    bool unserialize(const std::vector<uint8_t>& data) {
        if (!fn_retro_unserialize || data.empty()) return false;
        s_instance_ = this;
        return fn_retro_unserialize(data.data(), data.size());
    }
    /// Soft reset (retro_reset).
    void reset() {
        if (!fn_retro_reset) return;
        s_instance_ = this;
        fn_retro_reset();
    }

    // ----- Input injection (R2 base) ----------------------------------------
    // Per-port button bitfield read by s_input_state. Bit i = RETRO_DEVICE_ID_
    // JOYPAD_* id i (B=0, Y=1, SELECT=2, START=3, UP=4, DOWN=5, LEFT=6, RIGHT=7,
    // A=8, X=9, L=10, R=11). hash(0) = no buttons (today's behaviour).

    void     set_input(int port, uint16_t buttons) { if (port >= 0 && port < kPorts) input_[port] = buttons; }
    uint16_t input(int port) const { return (port >= 0 && port < kPorts) ? input_[port] : 0; }

    // ----- Opciones de core (EM-7.1, ) ----------------------------------
    //
    // libretro las llama «variables»: pares clave/valor que el core declara al
    // arrancar (SET_VARIABLES) y consulta cuando las necesita (GET_VARIABLE).
    // Son las que dan «sin límite de sprites» —el anti-flicker— y el
    // overclock donde el core lo ofrece.
    //
    // HASTA ACÁ NO ESTABAN SOPORTADAS: `GET_VARIABLE` devolvía false siempre y
    // el core caía a sus defaults internos. La fontanería que  daba por
    // existente era el hook, no la respuesta.
    //
    // SE APLICAN AL INICIALIZAR Y NO CAMBIAN EN VIVO, a propósito. El core lee
    // sus opciones una sola vez porque `GET_VARIABLE_UPDATE` contesta «no
    // cambiaron»: devolver «sí» ahí hacía que Genesis Plus GX re-aplicara las
    // opciones cada frame y reinicializara el chip de sonido — audio mudo, un
    // defecto que ya se pagó una vez. Cambiar una opción exige reiniciar la
    // sesión, y eso es lo honesto: la alternativa es un estado a medio aplicar
    // que nadie puede explicar.

    /// Fija el valor de una opción ANTES de `init`. Después de `init` queda
    /// guardada pero el core ya leyó las suyas.
    void set_core_option(const std::string& key, const std::string& value) {
        core_options_[key] = value;
    }
    /// El valor elegido, o "" si el frontend no fijó ninguno (y ahí manda el
    /// default del core).
    std::string core_option(const std::string& key) const {
        const auto it = core_options_.find(key);
        return it == core_options_.end() ? std::string() : it->second;
    }
    void clear_core_options() { core_options_.clear(); }

    ///  EM-7.4: parche IPS/BPS del usuario. Se aplica al buffer de la ROM
    /// —nunca al archivo— antes de dárselo al core, así que hay que fijarlo
    /// ANTES de `init`. Vacío = ninguno.
    void set_patch_path(const std::string& p) { patch_path_ = p; }

    /// Las opciones que el CORE declaró, en orden: `(clave, descripción)`. Sale
    /// de `SET_VARIABLES`, así que está poblada después de `init` y es lo que
    /// un frontend necesita para ofrecerlas sin hardcodear la lista de ningún
    /// core (BYOC: no sabemos cuál van a usar).
    const std::vector<std::pair<std::string, std::string>>& declared_options() const {
        return declared_options_;
    }

private:
    static constexpr int kPorts = 2;
    uint16_t input_[kPorts] = { 0, 0 };

    // ----- Function pointers loaded from the core DLL -----
    void (*fn_retro_set_environment)(retro_environment_t)            = nullptr;
    void (*fn_retro_set_video_refresh)(retro_video_refresh_t)        = nullptr;
    void (*fn_retro_set_audio_sample)(retro_audio_sample_t)          = nullptr;
    void (*fn_retro_set_audio_sample_batch)(retro_audio_sample_batch_t) = nullptr;
    void (*fn_retro_set_input_poll)(retro_input_poll_t)              = nullptr;
    void (*fn_retro_set_input_state)(retro_input_state_t)            = nullptr;
    void (*fn_retro_init)()                                          = nullptr;
    void (*fn_retro_deinit)()                                        = nullptr;
    bool (*fn_retro_load_game)(const retro_game_info*)               = nullptr;
    void (*fn_retro_unload_game)()                                   = nullptr;
    void (*fn_retro_run)()                                           = nullptr;
    void*  (*fn_retro_get_memory_data)(unsigned)                     = nullptr;
    void   (*fn_retro_cheat_set)(unsigned, bool, const char*)        = nullptr;
    void   (*fn_retro_cheat_reset)()                                 = nullptr;
    size_t (*fn_retro_get_memory_size)(unsigned)                     = nullptr;
    void (*fn_retro_get_system_info)(retro_system_info*)             = nullptr;
    void (*fn_retro_get_system_av_info)(retro_system_av_info*)       = nullptr;
    // Savestates + reset (R2 base)
    size_t (*fn_retro_serialize_size)()                              = nullptr;
    bool (*fn_retro_serialize)(void*, size_t)                        = nullptr;
    bool (*fn_retro_unserialize)(const void*, size_t)               = nullptr;
    void (*fn_retro_reset)()                                         = nullptr;

    // E-1 (): la ABI v1 del fork, resuelta en load_symbols(). Ausente en un
    // core stock — ver has_ayther_v1() arriba.
    /// Directorio que el core recibe como SYSTEM/SAVE/CONTENT directory. Es el
    /// de la ROM, y no un "." (el CWD del proceso, que cambia con quien
    /// invoque): ahí es donde el core busca los BIOS —`bios_CD_U.bin` y
    /// compañía para Sega CD, `bios_MD.bin`, `ggenie.bin`— y donde el propio
    /// core cae por defecto cuando el frontend no contesta.
    std::string system_dir_ = ".";
    std::string patch_path_;   ///<  EM-7.4: parche del usuario (IPS/BPS)
    /// EM-7.1: lo que el frontend eligió, y lo que el core declara ofrecer.
    std::map<std::string, std::string> core_options_;
    std::vector<std::pair<std::string, std::string>> declared_options_;

    /// El medio cargado es una imagen de DISCO (ver cd_media()).
    bool cd_media_ = false;

    ayther_get_interface_fn    fn_ayther_get_interface_ = nullptr;
    const ayther_interface_v1* ayther_api_              = nullptr;

    // E-6 (): el delta del último `run_frame()`, y si es utilizable.
    ayther_frame_delta_v1 last_delta_{};
    bool                  last_delta_ok_ = false;
    void poll_frame_delta_();

    /// Cuerpo común de las cuatro lecturas del VDP: el tamaño lo declara el
    /// core (query_region), no el Engine.
    AytherReadResult read_vdp_region_(uint32_t region, void* out,
                                      const ayther_frame_snapshot_v1& s) const;

    CoreLoader loader_;
    VideoCb    video_cb_;
    AudioCb    audio_cb_;

    void*    ram_ptr_      = nullptr;
    size_t   ram_size_     = 0;
    double   fps_          = 60.0; // filled by load_rom() from retro_system_av_info
    unsigned pixel_format_ = 2;   // default: RGB565 (Genesis Plus GX)
    bool     running_      = false;

    // ----- Static trampolines (libretro needs plain C callbacks) -----
    static RetroRunner* s_instance_;

    static bool   s_environment(unsigned cmd, void* data);
    static void   s_video_refresh(const void* data, unsigned w, unsigned h, size_t pitch);
    static void   s_audio_sample(int16_t left, int16_t right);
    static size_t s_audio_sample_batch(const int16_t* data, size_t frames);
    static void   s_input_poll();
    static int16_t s_input_state(unsigned port, unsigned device, unsigned index, unsigned id);

    bool load_symbols();
    bool load_rom(const std::string& rom_path);
};
