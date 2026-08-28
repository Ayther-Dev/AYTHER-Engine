// ---------------------------------------------------------------------------
// ayther_session.cpp — AytherSession implementation (R2).
//
// Owns the whole deterministic pipeline behind one object. Every opaque
// ayther_core handle is held as an ayther::unique_handle inside Impl (the pimpl),
// so nothing leaks on teardown or hot-reload (§4.1). This is the permanent home
// of the RAII primitive (risk ).
//
// The per-frame logic in step() is the pipeline that used to live inline in the
// player's main loop; here it is motor-owned and produces a FrameView the
// frontend renders. Audio output stays inside the session.
// ---------------------------------------------------------------------------

#include "ayther_env.h"
#include "ayther_session.h"
#include "panorama_cover.h"
#include "cram_palette.h"        //  EM-9.4: la CRAM, con su oraculo   // : la regla de cobertura, testeable sin ROM
#include "failure_escalation.h"   // : cuando dejar de intentar
#include "audio_seq_anchor.h"   // : anclas de Secuencia con reclamo
#include "parallax_bands.h"       //  EM-8.0: la columna de nivel por banda
#include "pano_bands.h"           // : el voto de la Panoramica por banda

#include "ayther_background_export.h"      // BackgroundExporter (Fondos, Componentes)
#include "ayther_components_toml.h"        // parse animations.toml / audio_events.toml
#include "ayther_unique_handle.h"          // ayther::unique_handle
#include "audio_player.h"                  // AudioPlayer (motor-owned audio out)
#include "audio_live_resume.h"             // : reanudación live con offset
#include "voice_router.h"                  // ChannelRouter (): componer en vez de mutear
#include "libretro_host/retro_runner.h"    // RetroRunner (emulator host)
#include "rewind_buffer.h"                 // RewindBuffer (R6)
#include "ayther_recording.h"              // AytherRecording (R7)
#include "ayther_video.h"                  // VideoClip: el paso-video ()
#include "ayther_core_ffi.h"                // ayther_sf2_* ()

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>


// E-5 (): los accessors legacy quedaron [[deprecated]] porque su reemplazo
// es la ABI, pero el camino de core STOCK sigue siendo válido y es el fallback
// deliberado de cada dual-path. Marcar esos usos —y sólo esos— deja el build
// limpio de warnings sin apagar el aviso para un caller nuevo que los use por
// distracción, que es de quien protege la deprecación.
#define AYTHER_LEGACY_READ_BEGIN     _Pragma("clang diagnostic push")     _Pragma("clang diagnostic ignored \"-Wdeprecated-declarations\"")
#define AYTHER_LEGACY_READ_END _Pragma("clang diagnostic pop")

namespace ayther {

// ---------------------------------------------------------------------------
// Per-handle RAII typedefs — declared in this TU so ayther_unique_handle.h need
// not depend on ayther_core_ffi.h.
// ---------------------------------------------------------------------------
namespace {
using TileHasherPtr   = unique_handle<AytherTileHasher,        &ayther_tile_hasher_free>;
using TileSubPtr      = unique_handle<AytherTileSubstitutor,   &ayther_tile_sub_free>;
using SpriteHasherPtr = unique_handle<AytherSpriteHasher,      &ayther_sprite_hasher_free>;
using SpriteSubPtr    = unique_handle<AytherSpriteSubstitutor, &ayther_sprite_sub_free>;
using PoseSubPtr      = unique_handle<PoseSetSubstitutor,      &ayther_pose_sub_free>;
using TweenPtr        = unique_handle<TweenPlayer,            &ayther_tween_free>;
using AudioHasherPtr  = unique_handle<AytherAudioHasher,       &ayther_audio_hasher_free>;
using AudioEventPtr   = unique_handle<AytherAudioEventDetector, &ayther_audio_event_free>;
using AudioSubPtr     = unique_handle<AytherAudioSubstitutor,  &ayther_audio_sub_free>;
using ScriptPtr       = unique_handle<AytherScriptEnv,         &ayther_script_free>;
using PackPtr         = unique_handle<AyArchive,               &ayther_pack_close>;
using BgStitcherPtr   = unique_handle<AytherBgStitcher,        &ayther_bg_stitcher_free>;
using UnwrapPtr       = unique_handle<AytherScrollUnwrapper,   &ayther_scroll_unwrapper_free>;

// Per-frame fixed capacities (match the original player main loop).
constexpr uint32_t kMaxTileOccs   = 2048;
constexpr uint32_t kMaxTileSubs   = 2048;
constexpr uint32_t kMaxSpriteOccs = 256;
constexpr uint32_t kMaxAudioOccs  = 256;
// Tiles ÚNICOS (patrón+paleta) catalogados por frame, COMPARTIDO por los tres
// planos. Un plano del VDP llega a 4096 celdas, así que el peor caso real son
// ~4096 claves distintas por plano; 8192 cubre A+B+Window de sobra en juegos
// reales y el sobrante se REPORTA (antes se descartaba en silencio).
constexpr uint32_t kMaxPlaneTileOccs = 8192;
constexpr uint32_t kMaxPlaneTileSubs = 512;    // overlays HD de tiles de plano por frame (Fase 2c)
constexpr uint32_t kMaxPlaneCells    = 4096;   // celdas de plano visibles/frame (sync viewport↔Capas)
constexpr uint32_t kMaxOverrides  = 256;

// Replay keyframe spacing (R7d/R7e): un savestate cada N frames acota el seek a
// ≤ N run_frame "bare". 300 @ ~60fps = 5 s → seek ≤ ~30 ms (imperceptible) y, al
// hornearlos en el .arp comprimidos, ~12 keyframes/min: balance disco/RAM/latencia.
constexpr uint32_t kReplayKeyInterval = 300;

/// Todos los canales que la máscara sabe nombrar: FM 0-5 · PSG 0-3 · PCM 0-7.
///
/// Existe con nombre porque el valor anterior —`0x3FF`, los diez de FM y PSG—
/// estaba escrito a mano en cuatro lugares que significan «mutear todo salvo
/// esto». Con el chip PCM adentro, cada uno de esos literales habría dejado sus
/// ocho canales SONANDO en un solo, sin que nada fallara.
constexpr uint32_t kAllChannels = 0x3FFFFu;   // 18 bits

/// Los canales que el ROUTER de voces () rinde por su cuenta: los seis del
/// FM —el sexto en modo DAC incluido, que el router también reproduce— y los
/// cuatro del PSG. Es exactamente lo que hay que callar en el core cuando el
/// bloque del router se SUMA en vez de ocupar el lugar (, Sega CD): lo que
/// queda sonando del chip es lo que el router no sabe hacer.
constexpr uint32_t kRouterChannels = 0x3FFu;   // FM 0-5 · PSG 0-3

inline uint32_t chan_bit(uint8_t chip, uint8_t channel) {
    if (chip == 0) return static_cast<uint32_t>(1u << channel);        // FM  0-5
    if (chip == 1) return static_cast<uint32_t>(1u << (6 + channel));  // PSG 0-3
    if (chip == 3) return static_cast<uint32_t>(1u << (10 + channel)); // PCM 0-7
    return 0;
}
}  // namespace

// ---------------------------------------------------------------------------
// Impl — owns every motor handle + the per-frame buffers FrameView points into.
// ---------------------------------------------------------------------------
struct AytherSession::Impl {
    RetroRunner runner;                       // emulator host
    std::string core_path, rom_path;

    TileHasherPtr   tile_hasher;              // ayther_core handles (RAII)
    TileSubPtr      tile_sub;
    TileSubPtr      plane_sub;                // Fase 2c: catálogo hash→asset de tiles de plano
    SpriteHasherPtr sprite_hasher;
    SpriteSubPtr    sprite_sub;
    PoseSubPtr      pose_sub;       // CU-AN multi-sprite: sustitución por firma de pose
    TweenPtr        tween;          // CU-AN in-betweens: playback por tiempo
    AudioHasherPtr  audio_hasher;
    AudioEventPtr   audio_event_det;   // C-A2: eventos por comandos de chip (recording-céntrico)
    AudioSubPtr     audio_sub;
    ScriptPtr       script;
    PackPtr         pack;
    std::string     pack_path;

    bool        audio_enabled = false;        // HD audio output (motor-owned)
    bool        vram_warned   = false;        // one-shot: core exposes no VRAM
    bool        poke_dirty    = false;        // M5: navegada por poke → REC off
    ///  EM-7.3: cheats del JUGADOR, reaplicados cada frame. Un vector y no
    /// un mapa: son unos pocos, el orden es el que el jugador los agregó, y dos
    /// cheats sobre la misma dirección son un conflicto suyo que no nos toca
    /// resolver.
    struct CheatEntry { uint32_t address; uint16_t value; };
    std::vector<CheatEntry> cheats;
    AudioPlayer audio;

    // Modo 3 (RAM anchoring): perfil de juego + sustitución HD por instancia.
    // resolve() corre en produce_frame con la cámara del plano A leída del VDP
    // (mismo camino que el resolver de Fase 2c, validado por tools/mode3_spike).
    Mode3Resolver mode3;

    // Fondos (Componentes): captura del stitcher para el export por capa. Con
    // bg_capture_on, produce_frame acumula las celdas visibles de A/B en espacio
    // de nivel; los unwrappers (uno por eje/plano) se crean lazy al primer frame
    // con geometría de plano (necesitan el período wpx/hpx de los VDP regs).
    bool         bg_capture_on = false;
    /// : corte de escena detectado (delta de scroll NO físico) → el stitch
    /// se CONGELA (conserva el nivel acumulado; la escena nueva no contamina).
    /// Se re-arma al (re)iniciar la captura.
    bool         bg_scene_cut  = false;
    BgStitcherPtr bg_st;
    /// TODAS las parejas (posición, hash) observadas por plano, acumuladas en
    /// paralelo al stitcher. El stitcher guarda códigos de nametable (para
    /// re-dibujar); esto guarda la identidad con la que la Panorámica se
    /// RECONOCE en runtime.
    ///
    /// Son TODAS y no la primera de cada posición: una celda animada muestra
    /// hashes distintos en la MISMA posición de nivel, y quedarse con el primero
    /// deja a los demás estados sin posición conocida — al anclar votan por
    /// cualquier otro lugar donde ese hash aparezca. Medido en Aladdin: con la
    /// primera sola, la moda del voto llegaba al 27% y no anclaba ni un frame de
    /// los que fallaban; con todas, al 100%.
    std::set<std::pair<uint64_t, uint64_t>> bg_hash[3];   // (poskey, hash)
    /// : el hash que la LÁMINA DIBUJA en cada posición — el que
    /// corresponde al código que el PNG conserva (`Cell::last` del stitcher,
    /// `background.rs`), no cualquiera de los que pasaron por ahí.
    ///
    /// Existe porque `bg_hash` guarda TODAS las lecturas (cada estado de una
    /// celda animada tiene que poder anclar) y el PNG guarda UNA. Verificar la
    /// cobertura contra el conjunto entero declara «100 %» sobre una lámina que
    /// muestra otro tramo del nivel: medido en Sonic 3 & K f2092, anclada con
    /// cobertura 100 % y el recorte mostrando Angel Island en una cueva.
    ///
    /// Se actualiza en el MISMO punto y con la MISMA política que el stitcher
    /// —último gana— así que los dos no se pueden separar sin que alguien
    /// cambie las dos líneas a la vez.
    std::unordered_map<uint64_t, uint64_t> bg_hash_drawn[3];
    int32_t bg_camx[3] = {}, bg_camy[3] = {};
    bool    bg_cam_ok[3] = {};
    // : bg_uyB — el plano B scrollea en vertical por SU entrada de VSRAM
    // (la impar); usar la de A dejaba al stitcher ciego a la subida de GA (la
    // tira quedaba clavada en 28 filas y el contenido nuevo pisaba al viejo).
    UnwrapPtr    bg_uxA, bg_uyA, bg_uxB, bg_uyB;
    // : cámara de CONTENIDO — juegos que scrollean reescribiendo la
    // nametable (GA sube el plano B corriendo el contenido en VRAM con el
    // registro fino volviendo a 0) mueven el nivel sin mover los registros.
    // Se detecta comparando la grilla visible contra la del frame anterior y
    // el delta extra (en CELDAS) se acumula acá; las coordenadas de NIVEL
    // (stitcher + bg_hash) suman este offset — la lectura de nametable sigue
    // por registros, que es lo físico.
    int32_t bg_content_col[3] = {}, bg_content_row[3] = {};
    std::vector<uint16_t> bg_prev_grid[2];   // codes visibles del frame previo (A/B)
    int32_t bg_prev_col[2] = {}, bg_prev_row[2] = {};
    bool    bg_prev_ok[2] = {};
    //  EM-8.0: la cámara ABSOLUTA de cada banda de parallax, por plano. El
    // plano B lleva una entrada de Hscroll por banda, así que «columna de nivel»
    // depende de la FILA; con una cámara única las bandas se apilan unas sobre
    // otras. Tiene estado porque el des-enrollado lo pide: cuántas vueltas dio
    // una banda no se deduce de un solo frame.
    ayther::BandCameras bg_bands[2];
    // : frames seguidos con la cámara del plano QUIETA — con quietud se
    // leen también las últimas columnas (el skip anti-streaming pierde el borde
    // derecho del extremo final del paneo: nadie vuelve a verlo).
    int     bg_static_frames[2] = {};

    // Audios C-A2 (Componentes): sustitución HD por EVENTO. Las ventanas se
    // resuelven sobre los eventos de la toma (resolve_audio_events); el bloque
    // de audio de produce_frame consulta mute_at/triggers_at por frame. El
    // cache de eventos permite re-resolver al (des)asignar sin re-detectar.
    AudioEventSubstitution        audio_evt;
    std::vector<AytherAudioEvent> audio_events_cache;

    // Animaciones C-S2 (Componentes): playback HD en fase. resolve() corre en
    // produce_frame sobre las sprite occurrences; FrameView publica los
    // AnimHdFrame y el renderer los dibuja con VkSprite::draw_anim.
    AnimationPlayer anim;

    // Lab authoring overrides (hash → asset + ref cromática E1 ).
    // Re-applied to the sprite substitutor every frame AFTER the Lua overrides,
    // so they persist across the per-frame clear and a script can still
    // override them per-context. ref_rgb {0,0,0} = sin ref → peak-hold gris.
    struct LabSpriteAssign { std::string asset; uint8_t ref_rgb[3] = {0, 0, 0}; };
    std::unordered_map<uint64_t, LabSpriteAssign> lab_sprite_overrides;
    // Sustitución de pose TRANSITORIA (todo elemento es una Pose): se aplican al
    // pose_sub cada frame ANTES del resolve y NUNCA se serializan. Con rel →
    // matching instanciado exacto. hd=true → su región limpia los sueltos.
    std::vector<AytherSession::PosePreview> preview_pose_overrides;
    // Ocultado COMPUESTO por hash (Posar): A completo → B con estos hashes
    // suprimidos (slots de las occs de A) → base = B sólo en sus rects.
    std::unordered_set<uint64_t> hidden_sprite_hashes;
    /// R-4 (): la visibilidad por ELEMENTO es una propiedad del inventario
    /// — set_hidden_elements guarda acá y los sets que YA leen los
    /// consumidores (hidden_sprite_hashes / plane_tiles_hidden) pasan a ser la
    /// UNIÓN canal-del-Lab ∪ elementos, recomputada en los setters
    /// (rebuild_hidden_sets). Un hash de sprite es inerte en el dominio de
    /// planos y viceversa, así que la unión no necesita ruteo. Costo asumido:
    /// un hash de plano en el set de sprites enciende hide_compose (un render
    /// B de más por produce) sin cambio visual — sólo mientras hay ocultos.
    std::vector<uint64_t> lab_sprite_hidden;   // canal existente (Posar/poses)
    std::vector<uint64_t> lab_plane_hidden;    // canal existente (Editar/Pintar)
    /// R-6 (): efectos por elemento, indexados por capa (misma identidad
    /// (capa,hash) que el ocultado — y por la misma razón: un gráfico puede
    /// existir en los dos dominios). El inventario los resuelve a fx_*.
    std::unordered_map<uint64_t, ElementEffect> element_fx[4];
    std::vector<AytherSession::HiddenElement> element_hidden;  // R-4: (capa,hash)
    /// : identidades a MEJORAR por software, por capa — UNIÓN de dos
    /// fuentes separables: el Lab (set_enhanced_elements, lista viva) y el pack
    /// ([[enhance]] de elements.toml, se vuelca en load_pack_into y se limpia
    /// al cambiar de pack). El inventario lee sólo `element_enhance`.
    // hash -> k (). Unión Lab ∪ pack: el Lab GANA si el mismo (capa,
    // hash) viene de los dos (insert no pisa) — lo que se está autorando
    // manda sobre lo horneado.
    std::unordered_map<uint64_t, uint8_t> element_enhance_lab[4];
    std::unordered_map<uint64_t, uint8_t> element_enhance_pack[4];
    std::unordered_map<uint64_t, uint8_t> element_enhance[4];
    bool element_enhance_any = false;
    void rebuild_enhance_sets() {
        element_enhance_any = false;
        for (int l = 0; l < 4; ++l) {
            element_enhance[l] = element_enhance_lab[l];
            element_enhance[l].insert(element_enhance_pack[l].begin(),
                                      element_enhance_pack[l].end());
            if (!element_enhance[l].empty()) element_enhance_any = true;
        }
    }
    void rebuild_hidden_sets() {
        hidden_sprite_hashes.clear();
        for (uint64_t h : lab_sprite_hidden) hidden_sprite_hashes.insert(h);
        plane_tiles_hidden.clear();
        for (uint64_t h : lab_plane_hidden) plane_tiles_hidden.insert(h);
        // Ruteo por CAPA: el mismo hash puede existir en ambos dominios (un
        // gráfico usado como sprite Y como tile de plano) — sin el ruteo,
        // ocultar el sprite ocultaba también las celdas (element_hidden_smoke).
        for (const AytherSession::HiddenElement& e : element_hidden)
            (e.layer == 3 ? hidden_sprite_hashes : plane_tiles_hidden).insert(e.hash);
        // La máscara por (plano,patrón,paleta) se re-arma en produce_frame con
        // las occurrences del frame; si ya no hay ocultos, apagar de inmediato.
        if (plane_tiles_hidden.empty()) plane_tile_suppress_any = false;
    }
    std::unordered_map<uint64_t, std::string> lab_tile_overrides;
    std::unordered_map<uint64_t, std::string> lab_audio_overrides;
    std::unordered_map<uint64_t, std::string> lab_plane_overrides;   // Fase 2c (tiles de plano)

    // Rewind (R6) — compressed savestate ring, captured at the end of step().
    RewindBuffer rewind;
    std::vector<uint8_t> rewind_scratch;   // reused serialize buffer (no per-frame alloc)
    float speed = 1.0f;                    // fast-forward multiplier (frontend reads it)

    // Recording (R7) — capture the input stream + an initial state for .arp.
    bool                  rec_active = false;
    std::vector<uint16_t> rec_inputs;
    std::vector<FrameStat> rec_stats;      // per-frame occurrence summary (R7b)
    std::vector<uint64_t> rec_hashes;      // flat sprite-hash history (R7c, CSR)
    std::vector<uint32_t> rec_hash_off;    // CSR offsets, starts {0}
    std::vector<uint64_t> rec_audio_hashes; // flat audio-hash history (.arp v7, CSR)
    std::vector<uint32_t> rec_audio_off;    // CSR offsets, starts {0}
    std::vector<uint8_t>  rec_initial;     // savestate at record start
    uint16_t              last_input0 = 0; // most recent port-0 input (logged each step)

    // Aislar capas (Lab Editar): máscara de capas DESEADA (bits A/B/Window/
    // Sprites). Se aplica SOLO en produce_frame (el frame visible); tras él, los
    // bits de SPRITES se restauran para que la re-simulación "bare" corra con
    // sprites completos → el status del VDP (overflow/colisión que el juego lee)
    // no cambia y el replay no diverge. Los planos no afectan status.
    uint8_t               layer_mask_want = 0xFF;
    bool                  layer_dim_want = false;    // atenuar capas no-sprite al 25% (0x108)
    uint8_t               suppress_want[16] = {0};   // slots SAT a ocultar (0x103)
    bool                  suppress_any = false;      // ¿algún slot suprimido?
    uint8_t               tile_suppress_want[512] = {0};  // celdas de tile a ocultar (0x104)
    bool                  tile_suppress_any = false;      // ¿alguna celda suprimida?
    // Tiles de PLANO a ocultar (0x105, Fase 2b). El Lab da hashes; la máscara por
    // (plano,patrón,paleta) se re-arma en produce_frame con las occurrences del
    // frame (idéntico esquema produce-only que tile_suppress). 3×1024 = 3072 bytes.
    std::unordered_set<uint64_t> plane_tiles_hidden;       // hashes ocultos (del Lab)
    // Plane SETS (Pintar Fase C): sustitución HD por ELEMENTO multi-tile.
    struct PlaneSetDef {
        uint8_t  plane = 0;
        uint16_t w_cells = 0, h_cells = 0;
        std::vector<AytherSession::PlaneSetMember> members;
        std::string asset;
        /// Referencia del tinte E1 (promedio RGB 0-255 de la línea CRAM del
        /// elemento al capturarlo). {0,0,0} = sin referencia → quad sin tinte.
        uint8_t ref_rgb[3] = { 0, 0, 0 };
        /// : re-anclaje del HUD al ensanchar. Ver PackPlaneSet::off_x.
        int16_t off_x = 0, off_y = 0;
    };
    std::unordered_map<uint64_t, PlaneSetDef> plane_sets;
    // ANIMACIÓN (): secuencia de plane sets con RELOJ PROPIO. Ver el
    // header para por qué es reproductor y no seguidor del contenido.
    struct PlaneSeqDef {
        std::vector<uint64_t>    steps;    ///< ids de plane set, EN ORDEN
        std::vector<std::string> assets;   ///< asset por paso ("" = el del set)
        std::vector<uint16_t>    durs;     ///< frames por paso (0 = kSeqDefaultDur)
        uint32_t                 total = 0;  ///< suma de duraciones, cacheada
    };
    /// Cadencia por defecto de un paso sin `~dur`: ≈7,5 fps, el mismo hold del
    /// player de preview del Lab (kKfHoldTicks) — así lo que se autoró mirando
    /// el preview se ve igual en el runtime.
    static constexpr uint16_t kSeqDefaultDur = 8;
    std::unordered_map<uint64_t, PlaneSeqDef> plane_seqs;
    /// Índice inverso set → [(seq, paso)]. El matcher entrega ids de SET; esto
    /// dice si ese set pertenece a una Animación. Ordenado para que, ante
    /// empate, siempre gane la misma (determinismo, no orden de hash).
    std::unordered_map<uint64_t, std::vector<std::pair<uint64_t, uint32_t>>> set_to_seq;
    /// Reloj por Animación. `anchor` es el frame de juego que corresponde al
    /// t=0 del ciclo; la posición se recalcula por aritmética ABSOLUTA, nunca
    /// con un acumulador (mismo criterio que el video de la Cinemática: un
    /// acumulador se desincroniza con el primer re-produce y no se recupera).
    struct PlaneSeqClock { int64_t anchor = -1; int64_t last_seen = -1; };
    std::unordered_map<uint64_t, PlaneSeqClock> seq_clocks;

    void plane_seq_reindex() {
        set_to_seq.clear();
        for (const auto& [id, d] : plane_seqs)
            for (uint32_t i = 0; i < d.steps.size(); ++i)
                set_to_seq[d.steps[i]].emplace_back(id, i);
        for (auto& [sid, v] : set_to_seq) std::sort(v.begin(), v.end());
    }

    /// Paso vigente de `id` en este frame, o UINT32_MAX si la Animación no
    /// existe o está vacía. Re-ancla cuando estuvo AUSENTE varios frames: si se
    /// re-anclara en cada aparición, un elemento que entra y sale de pantalla
    /// reiniciaría el ciclo constantemente y nunca pasaría del primer paso.
    uint32_t plane_seq_step(uint64_t id) {
        auto it = plane_seqs.find(id);
        if (it == plane_seqs.end() || it->second.steps.empty()) return UINT32_MAX;
        const PlaneSeqDef& d = it->second;
        PlaneSeqClock& c = seq_clocks[id];
        constexpr int64_t kGapFrames = 8;   // tolerancia de ausencia
        if (c.anchor < 0 || (int64_t)frame_index - c.last_seen > kGapFrames ||
            (int64_t)frame_index < c.last_seen)
            c.anchor = (int64_t)frame_index;
        c.last_seen = (int64_t)frame_index;
        const uint32_t total = d.total ? d.total : 1u;
        int64_t t = ((int64_t)frame_index - c.anchor) % (int64_t)total;
        if (t < 0) t = 0;
        return ayther::plane_sequence_step_at(d.durs.data(),
                                              (uint32_t)d.durs.size(),
                                              (uint64_t)t, kSeqDefaultDur);
    }
    // CUADRO (CU001): pantallas estáticas declaradas + estado del match.
    struct ScreenDef {
        uint8_t     mask = 0;
        float       min_match = 0.92f, max_extra = 0.08f;
        std::string asset;
        /// Firma y conteo POR CAPA. Separadas y no sumadas: sumarlas puede
        /// cancelarse (A sube en X y B baja en X y el total no se mueve) y,
        /// sobre todo, pierde CUÁL capa cambió — que es lo que hace falta para
        /// una pantalla con parallax, donde una capa está fija y la otra se
        /// desplaza. Un Cuadro sólo mira las capas que DECLARA: lo que pase en
        /// las otras no lo invalida.
        uint64_t    sig_plane[3]   = {0, 0, 0};
        uint32_t    cells_plane[3] = {0, 0, 0};
        std::unordered_map<uint32_t, uint64_t> cells;   // key(plano,col,fila) → hash
        ///  mecanismo 2: hashes DISTINTOS por capa, sin posición — el
        /// universo del gate por presencia. Derivado de `cells` al declarar.
        std::unordered_set<uint64_t> hashes_plane[3];
    };
    std::unordered_map<uint64_t, ScreenDef> screens;
    uint64_t        screen_active = 0;    // Cuadro vigente (0 = ninguno)
    uint64_t        screen_cand   = 0;    // candidato esperando confirmación
    int             screen_streak = 0;    // frames consecutivos del candidato
    /// El frame vino de un SALTO (scrub), no del avance continuo. La histéresis
    /// de 2 frames existe para el playback —un wipe puede acertar una firma por
    /// un frame suelto— pero al saltar sólo se produce el frame de destino: el
    /// streak se quedaba en 1 y el Cuadro no se activaba NUNCA mientras se
    /// navegaba a mano, que es justo como se autora.
    bool            screen_jump   = false;
    float           screen_score = 0.0f, screen_extra = 0.0f;
    ///  mecanismo 2: ids cuyo contenido está presente este frame.
    uint64_t        screen_presence[8] = {};
    uint32_t        screen_presence_n  = 0;
    AytherSpriteSub screen_sub{};         // el quad a pantalla completa
    uint32_t        screen_sub_n = 0;

    // CINEMÁTICA (CU004): una SECUENCIA ORDENADA de Cuadros. Lo que agrega
    // sobre un Cuadro suelto no es el dibujo —eso ya lo hace el Cuadro— sino el
    // ORDEN: desambigua dos pantallas idénticas que aparecen en cinemáticas
    // distintas, y da la semántica de cancelación del spec (si el jugador
    // aprieta Start y el juego salta a un menú, la secuencia se corta y se
    // re-evalúa la pantalla nueva).
    struct KinematicDef {
        std::vector<uint64_t>    steps;    ///< ids de Cuadro, EN ORDEN
        std::vector<std::string> assets;   ///< asset por paso ("" = el del Cuadro)
        /// Frame del clip en que arranca cada paso, cuando el asset es video
        /// (). Es lo que deja que UN video cubra varios pasos.
        std::vector<uint32_t>    video_offsets;
        uint32_t gap = 12;                 ///< frames tolerados sin Cuadro confirmado
        /// El video CICLA si es más corto que el tramo (ver KinematicMedia).
        bool        loop = false;
        /// Pista de audio del video (asset aparte: el IVF es sólo video).
        std::string audio;
        float       gain = 1.0f;        ///< volumen de la pista de la Cinemática
        float       game_gain = 1.0f;   ///< ducking de la banda sonora del juego
    };
    std::unordered_map<uint64_t, KinematicDef> kinematics;
    /// Índice inverso screen_id → [(kin_id, paso)]. El matcher de Cuadro entrega
    /// UN id por frame; esto dice qué cinemáticas lo esperan y en qué posición.
    std::unordered_map<uint64_t, std::vector<std::pair<uint64_t,uint32_t>>> screen_to_kin;
    uint64_t kine_active = 0;      ///< cinemática en curso (0 = ninguna)
    uint32_t kine_step   = 0;      ///< paso vigente dentro de la secuencia
    uint32_t kine_gap    = 0;      ///< frames seguidos sin Cuadro confirmado
    /// Último frame en que el cursor AVANZÓ. produce_frame NO es 1:1 con los
    /// frames emulados —corre de nuevo en el re-render bare del compose, en
    /// export_frame y en replay_invalidate— y el matcher de Cuadro sobrevive a
    /// eso porque es una función PURA del frame. Una máquina de estados no: sin
    /// este gate, re-producir el mismo frame haría avanzar la secuencia.
    int64_t  kine_last_frame = -1;

    // VIDEO del paso (). Los clips se abren perezosamente la primera vez que
    // un paso los pide y se quedan cacheados por ruta. Desde  lo cacheado
    // es el ÍNDICE y la fuente, no el archivo: un clip abierto ocupa un paquete
    // más un frame, mida el video 30 MB o 1 GB.
    std::unordered_map<std::string, std::unique_ptr<ayther::VideoClip>> videos;
    /// Fase del video, DELIBERADAMENTE fuera de lo que `kinematic_reset()`
    /// borra, y anclada por identidad `(cinemática, paso)` en vez de por un
    /// contador propio.
    ///
    /// El motivo es una cadena real: el Lab adelanta el playhead por reloj hasta
    /// 16 frames y recién ahí emite UN `replay_seek`; el motor lo resuelve con
    /// fast-forward bare y produce sólo el frame terminal; entonces
    /// `fnow != kine_last_frame + 1` y corre `kinematic_reset()`. Hoy eso es
    /// benigno porque la re-entrada por contenido cae en el mismo paso. Con un
    /// contador «frames desde que arrancó el paso» adentro del estado reseteable,
    /// CADA hipo de playback rebobinaría el video al inicio del plano.
    ///
    /// Con el ancla por identidad: si el paso sigue siendo el mismo, el ancla
    /// sobrevive y la posición se recalcula por aritmética ABSOLUTA
    /// (`frame_index - anchor`), sin acumulador que se desincronice.
    struct VideoPhase {
        uint64_t kin    = 0;
        uint32_t step   = 0;
        int64_t  anchor = -1;
    } vid;
    ayther::VideoFrameView vid_out{};
    bool                   vid_on = false;

    /// AUDIO de la Cinemática. El IVF es sólo video, así que la pista viaja
    /// como asset aparte y se reproduce con el mismo criterio de ancla que la
    /// imagen — pero NO se puede re-anclar por paso: la Cinemática avanza 44
    /// pasos en cuatro segundos y reiniciar el stream en cada uno sería un
    /// tartamudeo continuo. Entonces se arranca UNA vez y sólo se re-sincroniza
    /// cuando el ancla SALTA (un scrub), que es exactamente lo que hay que
    /// corregir. `still` cuenta ticks sin avanzar: pausado se corta, porque
    /// scrubbear cuadro a cuadro con la voz corriendo no es una previsualización
    /// de nada.
    struct VideoAudio {
        uint64_t kin    = 0;
        int64_t  anchor = 0;      ///< frame de juego que mapea al t=0 del asset
        bool     on     = false;
        int64_t  last_f = -1;
        int      still  = 0;
        float    gain   = 1.0f;   ///< la última ganancia aplicada al stream vivo
    } vaud;
    /// Clave del stream (dedup/corte). Constante: hay UNA Cinemática activa.
    static constexpr uint64_t kVideoAudioKey = 0xA17E'2600'0000'0002ull;

    void video_audio_stop() {
        if (vaud.on) audio.stop_sfx_by_key(kVideoAudioKey);
        // La banda sonora vuelve SIEMPRE a su volumen, aunque no hubiera pista
        // de Cinemática: el ducking es un préstamo con lifetime propio y
        // dejarlo bajado sería un bug silencioso que sobreviviría a la escena.
        audio.set_game_gain(1.0f);
        vaud = {};
    }
    void video_reset() {
        vid = {}; vid_out = {}; vid_on = false;
        video_audio_stop();
    }

    /// Resuelve el frame de video que corresponde a ESTE frame de juego.
    /// Declarado acá y definido fuera de la clase (necesita leer del pack).
    void video_tick(const std::string& path);

    void kinematic_reindex() {
        screen_to_kin.clear();
        for (const auto& [kid, d] : kinematics)
            for (uint32_t i = 0; i < d.steps.size(); ++i)
                screen_to_kin[d.steps[i]].emplace_back(kid, i);
        // Orden determinista: el arranque elige entre candidatos y un
        // unordered_map no garantiza por cuál empieza.
        for (auto& [sid, v] : screen_to_kin) std::sort(v.begin(), v.end());
    }
    void kinematic_reset() { kine_active = 0; kine_step = 0; kine_gap = 0; }

    /// Un tick de la secuencia. `sid` = el Cuadro CONFIRMADO este frame (0 = ninguno).
    void kinematic_tick(uint64_t sid) {
        if (kine_active) {
            auto it = kinematics.find(kine_active);
            if (it == kinematics.end()) { kinematic_reset(); }
            else {
                const auto& st = it->second.steps;
                if (kine_step >= st.size()) { kinematic_reset(); }
                else if (sid == 0) {
                    // TOLERANCIA. `screen_active` cae a 0 durante UN frame en
                    // toda transición limpia de Cuadro (la histéresis exige 2
                    // frames para confirmar el nuevo), y varios más si hay un
                    // wipe. Cancelar por «no hay Cuadro» rompería la secuencia
                    // en CADA paso legítimo.
                    if (++kine_gap > it->second.gap) kinematic_reset();
                } else if (sid == st[kine_step]) {
                    kine_gap = 0;                       // sigue en el mismo paso
                } else if (kine_step + 1 < st.size() && sid == st[kine_step + 1]) {
                    ++kine_step; kine_gap = 0;          // avanza
                } else {
                    kinematic_reset();                  // Cuadro AJENO: se rompió
                }
            }
        }
        if (!kine_active && sid) {
            // Arranque. Se permite entrar en CUALQUIER paso, no sólo el
            // primero: la posición sale del CONTENIDO de la pantalla, así que
            // saltar al medio de la cinemática con un scrub cae donde
            // corresponde. Ante empate gana el paso más temprano y, a igualdad,
            // el id menor — determinismo, no orden de hash.
            auto f = screen_to_kin.find(sid);
            if (f != screen_to_kin.end() && !f->second.empty()) {
                uint64_t bk = 0; uint32_t bs = UINT32_MAX;
                for (const auto& [kid, step] : f->second)
                    if (step < bs || (step == bs && kid < bk)) { bs = step; bk = kid; }
                if (bk) { kine_active = bk; kine_step = bs; kine_gap = 0; }
            }
        }
    }
    // PANORÁMICA (CU003): la tira del nivel de una capa + anclaje por contenido.
    struct PanoramaDef {
        uint8_t     plane = 0;
        int32_t     origin_x = 0, origin_y = 0;
        uint16_t    w_cells = 0, h_cells = 0;
        std::string asset;
        /// Luma CRAM de REFERENCIA (promedio de las 4 líneas al DEFINIR la
        /// tira, con la escena a niveles normales): el tinte del quad en vivo
        /// es luma_viva/ref — con la paleta fundida a negro la tira se apaga
        /// con la escena (f33–86 de la demo mostraba la tira a todo color
        /// sobre un fundido, reporte 2026-07-30). 0.30 = fallback razonable
        /// cuando al definir no hay CRAM iluminada (carga de proyecto/pack).
        /// Queda como FALLBACK del tinte cromático de abajo.
        double      ref_luma = 0.30;
        /// Referencia CROMÁTICA del tinte (2026-08-16): el mismo mecanismo E1
        /// que ya usan los sprites, llevado a la tira. La versión luma sólo
        /// APAGA y ENCIENDE — un atardecer que vira a naranja se veía como un
        /// oscurecimiento gris, y un amanecer no se veía en absoluto porque el
        /// factor estaba topado en 1.
        ///
        /// La referencia se captura ANCLADA (peak-hold en produce_frame), no
        /// al definir: define_panorama corre en cada sync del catálogo con la
        /// CRAM de la pantalla que esté viva — capturar ahí le pegaba a la
        /// panorámica del gameplay la paleta del TÍTULO y el cociente por
        /// canal convertía esa referencia ajena en un viraje rojizo con
        /// ganancia 2× (reporte 2026-08-19).
        ///
        /// `ref_w[p]` = cuánto aportaba la línea `p` al capturar (su luma). Se
        /// usa como PESO en los dos lados del cociente, y por eso una línea
        /// negra —o una que la tira no usa— no diluye la señal: es lo que hacía
        /// imperceptible un atardecer que sólo tocaba la línea del cielo,
        /// promediado contra otras tres que no cambiaban.
        double      ref_w[4]  = { 0, 0, 0, 0 };
        /// Agregado ponderado por canal al capturar: Σ_p ref_w[p]·rgb[p][c].
        double      ref_ch[3] = { 0, 0, 0 };
        bool        ref_chroma = false;   ///< hay referencia cromática utilizable
        /// Luma del frame ANCLADO en que se capturó la referencia vigente
        /// (peak-hold, como pal_luma_peak): el primer frame matcheado la fija
        /// y uno más luminoso la re-fija — así el fade-in de una transición
        /// sube la referencia con la escena hasta el nivel normal. 0 = nunca
        /// se capturó anclada.
        double      ref_peak = 0.0;
        /// Índice de ANCLAJE: sólo los hashes RAROS de la tira (los frecuentes
        /// no discriminan y multiplican el costo del voto).
        std::unordered_map<uint64_t, std::vector<std::pair<int32_t,int32_t>>> anchors;
        /// Índice de COBERTURA: (lx,ly) → hash, la tira entera. `anchors` sólo
        /// tiene los raros y no sirve para esto: una vez anclada la cámara hay
        /// que decidir, celda por celda, si lo que se ve ES la tira o es otra
        /// cosa dibujada sobre el mismo plano (un HUD, un primer plano). Se
        /// compara el hash observado contra el de la tira en esa posición de
        /// nivel — si no coincide, esa celda NO se cubre.
        /// Posición → hashes vistos ahí. Varios cuando la celda está animada.
        std::unordered_map<uint64_t, std::vector<uint64_t>> by_pos;
        uint32_t    total_cells = 0, rare_cells = 0;
        ///  EM-8.1: % de posiciones de la tira con UN solo hash. Bajo =
        /// tira ambigua (animada o contaminada por un barrido que cruzó de
        /// zona), y entonces la verificación de cobertura no vale para
        /// decidir qué dibujar donde nadie puede corregirlo.
        uint32_t    clean_pct = 0;
    };
    /// Clave de `by_pos`. Empaqueta dos int32 de posición de nivel.
    static uint64_t pano_key(int32_t lx, int32_t ly) {
        return ((uint64_t)(uint32_t)lx << 32) | (uint32_t)ly;
    }

    // -- : matcheo de celdas TOLERANTE AL REPALETADO ---------------------
    //
    // El hash de celda mezcla el ÍNDICE de línea CRAM al final del FNV1a del
    // patrón. Eso hace que un juego que reasigna la celda a otra línea —lo que
    // hacen los ciclos de día/noche que REPINTAN en vez de cambiar el contenido
    // de la línea— produzca un hash distinto para el mismo dibujo, y la tira se
    // despegue sola: dejan de votar las celdas, la cobertura no llega al piso y
    // la Panorámica no se dibuja.
    //
    // No se puede normalizar el lado de la TIRA porque no guarda la paleta:
    // `PanoramaCell` es hash + posición, y ese es también el formato del pack.
    // Lo que sí se puede es preguntar por las otras lecturas del hash OBSERVADO:
    // `ayther_plane_tile_hash_repalette` deshace la última vuelta y la rehace
    // bajo otra línea con aritmética exacta (el PRIME es invertible mod 2^64),
    // así que las cuatro variantes son exactas, no aproximadas.
    //
    // El camino DIRECTO se prueba siempre primero: sin repaletado —el caso
    // normal— esto no cuesta nada, y el trabajo extra lo pagan sólo las celdas
    // que ya iban a descartarse.

    /// Las 4 lecturas del hash `h` observado bajo la línea `pal`. `out[0]` es el
    /// hash tal cual. Vive en el header público (con su oráculo) porque la
    /// regla es del FORMATO del hash, no de la Panorámica.
    static void pano_hash_variants(uint64_t h, uint8_t pal, uint64_t out[4]) {
        ayther_plane_tile_hash_variants(h, pal, out);
    }

    /// Posiciones de anclaje de `h` en la tira, bajo cualquier línea. null = no
    /// es una celda de anclaje.
    static const std::vector<std::pair<int32_t, int32_t>>*
    pano_find_anchor(const PanoramaDef& pd, uint64_t h, uint8_t pal) {
        auto it = pd.anchors.find(h);
        if (it != pd.anchors.end()) return &it->second;
        uint64_t var[4];
        pano_hash_variants(h, pal, var);
        for (int i = 1; i < 4; ++i) {
            it = pd.anchors.find(var[i]);
            if (it != pd.anchors.end()) return &it->second;
        }
        return nullptr;
    }

    /// ¿La celda que la LÁMINA DIBUJA en esa posición ES la observada? ()
    ///
    /// La regla vive en `panorama_cover.h` —es del FORMATO de la tira, no de la
    /// sesión— y por eso tiene oráculo propio sin ROM. Acá sólo se le inyecta
    /// la función de variantes del core.
    static bool pano_pos_matches(const std::vector<uint64_t>& strip,
                                 uint64_t h, uint8_t pal) {
        return ayther::panorama_pos_matches(strip, h, pal,
                                            &ayther_plane_tile_hash_variants);
    }

    /// Construye la def desde las celdas crudas. Vive en Impl y no dentro de
    /// `AytherSession::define_panorama` porque `load_pack_into` TAMBIÉN la
    /// necesita y es método de Impl — no tiene el objeto público. Sin esto, la
    /// carga del pack tendría que duplicar el cálculo de rareza, con su
    /// constante de tuning, y las dos copias se irían separando.
    static PanoramaDef build_panorama(uint8_t plane, int32_t ox, int32_t oy,
                                      uint16_t w, uint16_t h,
                                      const AytherSession::PanoramaCell* cells,
                                      uint32_t n, const std::string& asset);
    std::unordered_map<uint64_t, PanoramaDef> panoramas;
    ///  fase 0: ancho lógico del ensanchado (0 = apagado). Es lo que PIDE
    /// el caller (el Lab, o el runtime); el gate de EM-8.2 puede pisarlo por
    /// frame sin destruirlo — apagar el gate tiene que devolver esto intacto.
    uint32_t wide_w = 0;
    ///  EM-8.2: el gate del pack. NULL cuando el pack no declara
    /// `[[widescreen]]`, que es el caso de todos los ya horneados: entonces
    /// `wide_w` manda solo y el ensanchado manual del Lab sigue funcionando.
    ///
    /// El gate es OBLIGATORIO y no un refinamiento: el área extendida sale de
    /// la lámina, y la lámina sólo existe donde el juego recorrió. Medido con
    /// `widescreen_spike`: en una toma quieta la racha dibujable es 0 por los
    /// cuatro lados. En un menú o una pantalla de título, ensanchar no muestra
    /// el nivel — muestra el vacío.
    struct WsGateDel {
        void operator()(WidescreenGate* g) const { ayther_widescreen_gate_free(g); }
    };
    std::unique_ptr<WidescreenGate, WsGateDel> wide_gate;
    /// Ancho EFECTIVO del frame: el del gate si opinó, si no el pedido.
    uint32_t wide_w_eff = 0;
    int32_t  pano_cam_x = 0, pano_cam_y = 0;
    /// : la cámara POR BANDA de la Panorámica ganadora. Con line-scroll un
    /// plano tiene bandas que se desplazan a distinto ritmo y una sola cámara
    /// no las explica (medido: Sonic 3 & K, 37 bandas en el plano B; Golden Axe
    /// ninguna en 40.854 frames). Vacío o de un solo elemento = el modelo de
    /// siempre, y la emisión sale idéntica.
    std::vector<BandCam> pano_bandcams;
    uint64_t pano_id = 0;
    uint8_t  pano_tint[3] = { 64, 64, 64 };   // Q2.6 (64 = 1.0): fundido del quad
    uint32_t pano_votes = 0, pano_cells = 0;
    ///  EM-8.1: qué fracción de las celdas visibles del plano EXPLICA la
    /// tira en la posición anclada (0-100). Es el mismo número que decide
    /// `explains`, guardado porque el área extendida le exige más que la nativa.
    uint32_t pano_cover = 0;
    bool     pano_valid = false;
    std::vector<AytherSpriteSub> pano_subs;   // un quad por TRAMO de la tira
    /// El aviso de catálogo lleno se emite UNA vez por sesión (lección de los
    /// stat storms: un fprintf por frame es peor que el problema).
    bool     plane_occ_warned = false;
    /// Último anclaje válido, para la continuidad temporal del voto (un hash
    /// raro repetido en otro tramo del nivel produciría outliers sueltos).
    /// NO participa cuando no hay referencia — tras un seek se resuelve por
    /// moda pura, que es lo que hace que el salto siga anclando bien.
    uint64_t pano_last_id = 0;
    int32_t  pano_last_x = 0, pano_last_y = 0;
    /// Modo HD del frontend. Gatea el matcher de sets: con el HD apagado la
    /// supresión de los originales dejaría agujeros. Antes vivía sólo en el
    /// Lab (que limpiaba los sets a mano); el runtime no tenía ninguno, así que
    /// apenas los sets viajaron en el pack pasar a Original mostraba los
    /// agujeros. Default ON: una sesión sin frontend que lo maneje ve el HD.
    bool hd_enabled = true;
    /// : qué sustituciones están encendidas, por subsistema. Todos en 1 por
    /// default — una sesión sin frontend que los maneje se comporta como antes.
    ///
    /// Vive al lado de `hd_enabled` y no lo reemplaza: aquél es la llave de luz
    /// de la casa (y además gatea la SUPRESIÓN de originales, que sin HD dejaría
    /// agujeros), y esto son las llaves de cada habitación.
    uint32_t subsystems_on = 0xFFFFFFFFu;
    /// : el último perfil que se APLICÓ. Es una pista, no la verdad — la
    /// verdad sigue siendo `subsystems_on` + los mutes, y `active_profile()`
    /// verifica esto contra ellos antes de devolverlo.
    ///
    /// Existe porque dos perfiles pueden tener el mismo efecto (uno recortado
    /// coincide con otro más chico), y ahí deducir el activo del estado
    /// devolvería cualquiera de los dos: el usuario eligió «Reinterpretado» y
    /// la UI le mostraría «Fiel». Guardar la elección lo resuelve sin
    /// introducir una segunda verdad, porque en cuanto el estado deja de
    /// coincidir la pista se descarta.
    std::string profile_hint;
    // -- Buses de audio () ----------------------------------------------
    /// Volumen por bus (índice = AudioBus). 1.0 = como se autoró.
    float bus_gain[kAudioBusCount] = {1.0f, 1.0f, 1.0f, 1.0f};
    /// Silencio por bus. Distinto de apagar el subsistema (): apagar el
    /// subsistema devuelve el ORIGINAL, silenciar el bus calla la categoría
    /// entera —HD y original— porque la intención es «no quiero música», no
    /// «prefiero la música del juego».
    bool  bus_mute[kAudioBusCount] = {false, false, false, false};
    /// : el bus DECLARADO por el pack, por firma (`audio_events.toml`).
    /// Sólo las que lo declararon: una firma ausente cae al default, que es
    /// otra cosa que «el pack dijo sin clasificar».
    std::unordered_map<uint64_t, AudioBus> audio_event_bus;

    /// El bus de un sonido, por su firma.
    ///
    /// Sale del «Tipo» que el autor le puso a la Secuencia que lo contiene. Lo
    /// que NO tiene Secuencia —una asignación por firma suelta, que hoy es el
    /// camino más usado— cae en **Efectos** (decisión de David, 2026-08-14):
    /// un sonido suelto es un efecto hasta que alguien diga lo contrario, y es
    /// la opción que no obliga a clasificar cien firmas para poder bajar la
    /// música.
    ///
    /// Una Secuencia SIN clasificar se queda en `Unclassified` y no hereda
    /// Efectos: ahí el autor tiene el control y no dijo nada; suponer sería
    /// meterle su música en el bus equivocado.
    AudioBus bus_of_signature(uint64_t sig) const {
        for (const auto& sq : audio_seq_subs) {
            if (sq.trigger_signature == sig ||
                std::find(sq.signatures.begin(), sq.signatures.end(), sig)
                    != sq.signatures.end())
                return sq.bus;
        }
        // : el bus que el PACK declara para esta firma (`audio_events.toml
        // → bus`). Va después de las Secuencias —que son más específicas— y
        // ANTES del default: un pack que clasificó sus eventos ya dijo de qué
        // son, y caer en Efectos ignoraría lo que el autor escribió.
        if (const auto it = audio_event_bus.find(sig); it != audio_event_bus.end())
            return it->second;
        return AudioBus::Sfx;
    }
    float bus_gain_of(AudioBus b) const {
        return bus_gain[static_cast<uint32_t>(b) % kAudioBusCount];
    }
    bool bus_is_muted(AudioBus b) const {
        return bus_mute[static_cast<uint32_t>(b) % kAudioBusCount];
    }
    /// ¿Se aplica el reemplazo de este subsistema en este frame? Un solo lugar
    /// donde preguntarlo: si el gate se copia en cada punto de uso, el día que
    /// se agregue una condición va a quedar puesta en la mitad de ellos.
    bool sub_on(Subsystem s) const {
        return (subsystems_on & subsystem_bit(s)) != 0;
    }
    uint8_t               plane_tile_suppress_want[3 * 1024] = {0};
    bool                  plane_tile_suppress_any = false;

    // Replay acceleration (R7d) — sin esto, replay_seek re-simulaba [0,frame)
    // desde el estado inicial en CADA llamada (scrub = O(frame), pegaba la CPU).
    //   • replay_pos: frame en el que quedó la máquina viva tras el último
    //     replay_seek (post-produce_frame); -1 = desconocido. Permite continuar
    //     hacia adelante sin unserialize.
    //   • replay_keys: keyframes (frame → savestate RAW que reproduce ese frame),
    //     capturados cada kReplayKeyInterval. Un seek arranca del más cercano ≤
    //     target, acotando el re-sim a ≤ kReplayKeyInterval frames.
    //   • replay_rec: identidad de la grabación cacheada (el caller resetea al
    //     cargar/dividir, ver replay_reset()).
    const AytherRecording*                   replay_rec = nullptr;
    int                                      replay_pos = -1;
    std::map<uint32_t, std::vector<uint8_t>> replay_keys;
    std::vector<uint8_t>                     kf_scratch;   // savestate horneado descomprimido (on-demand)

    // Keyframes horneados captados durante la grabación (crudos; se comprimen al
    // cerrar la toma en take_recording → AytherRecording::keyframes).
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> rec_keyframes;

    // Seek en chunks (R7e): reparte un seek frío (decenas de miles de frames)
    // entre frames de UI para no congelar la app. Mantiene la máquina a media
    // cadena bare entre llamadas; el resultado es idéntico a replay_seek.
    struct ChunkSeek {
        const AytherRecording* rec    = nullptr;
        bool                   active = false;
        uint32_t               target = 0;
        uint32_t               cur    = 0;   // próximo frame bare a correr
        uint32_t               start  = 0;   // origen del seek (para el progreso)
    } chunk;

    // Migración R7e (hornear keyframes en una toma vieja, troceado).
    struct Bake { const AytherRecording* rec = nullptr; bool active = false; uint32_t cur = 0; } bake;
    // Migración v8 (re-hornear la historia de hashes de sprites, troceado).
    struct HBake {
        const AytherRecording* rec    = nullptr;
        bool                   active = false;
        uint32_t               cur    = 0;
        std::vector<uint64_t>  hashes;   // CSR en construcción
        std::vector<uint32_t>  off;      // arranca {0}
    } hbake;
    bool replay_quiet = false;   // produce_frame omite la salida de audio (warm/bake)

    // Vista previa de audio (panel Capas): durante la captura los callbacks corren
    // en "modo captura" — el video no hashea (rápido) y el audio no sale al device;
    // si cap_collect, se acumula al buffer la MEZCLA de cada frame (sin filtrar por
    // hash: el audio del replay no es byte-reproducible → aislar por hash no sirve).
    bool                  cap_active  = false;
    bool                  cap_collect = false;
    std::vector<int16_t>  cap_pcm;


    // Framebuffer snapshot set by the video callback during run_frame().
    struct Snap { const void* data = nullptr; unsigned w = 0, h = 0; size_t pitch = 0; } snap;

    // Per-frame buffers (FrameView points into these; valid until next step()).
    AytherTileOccurrence   tile_occs[kMaxTileOccs];
    AytherSpriteOccurrence sprite_occs[kMaxSpriteOccs];
    AytherAudioOccurrence  audio_occs[kMaxAudioOccs];
    std::vector<AytherAudioWrite> chip_writes;  // raw FM/PSG bus writes this frame (copiado del core tras el produce)
    std::vector<AytherAudioEvent> audio_events; // eventos detectados por el último analyze_audio_events
    // : el otro camino del audio. El chip PCM de Sega CD no tiene bus
    // expuesto — llega ya tipificado por poll_audio_events — así que no puede
    // viajar en `chip_writes`. Se desempaqueta una vez por frame y entra al
    // detector en la MISMA llamada que las escrituras.
    std::vector<ayther_audio_event_v1> audio_evt_scratch;  // buffer crudo del polleo
    std::vector<AytherPcmEvent>        pcm_events;         // los de PCM, desempaquetados
    bool pcm_schema_warned = false;
    // C-A3b: sustitución por evento. Asignaciones firma→asset HD; flag de preview;
    // subs activos + máscara aplicada este produce (FrameView).
    std::unordered_map<uint64_t, std::string> audio_event_assign;
    std::unordered_map<uint64_t, uint32_t>    audio_event_channels;  // canales por firma (de la carga; fallback si no se reanalizó)
    // SECUENCIAS (Mezclar): firma disparadora → ventana relativa {duración,
    // loop}. Al rising-edge de la firma, el runtime abre una ventana de
    // range-mute de sus canales + HD (loop hasta cerrarla). 0 = sub clásica.
    std::unordered_map<uint64_t, uint32_t>    audio_event_duration;
    std::unordered_map<uint64_t, bool>        audio_event_looping;
    // : tail por firma — cuántos frames puede seguir el HD DESPUÉS de su
    // end_frame (0 = corte exacto). AUSENTE del mapa = ILIMITADO: el legacy
    // (non-loop drena entero) se conserva para lo ya autorado/horneado; las
    // entradas de Secuencia de los bakes nuevos lo escriben explícito.
    std::unordered_map<uint64_t, uint32_t>    audio_event_tail;
    static constexpr uint32_t kTailUnlimited = UINT32_MAX;
    /// : fade de fin por firma — 0 / ausente = sin fade (manda ).
    std::unordered_map<uint64_t, uint32_t>    audio_event_fade;
    uint32_t fade_of(uint64_t sig) const {
        const auto it = audio_event_fade.find(sig);
        return it == audio_event_fade.end() ? 0u : it->second;
    }
    /// : ganancia AUTORADA por firma (Secuencia). Ausente = 1.0 — el mixer
    /// aplicaba ese valor fijo, asi que un pack sin el dato suena igual.
    std::unordered_map<uint64_t, float>       audio_event_gain;
    float gain_of(uint64_t sig) const {
        const auto it = audio_event_gain.find(sig);
        return it == audio_event_gain.end() ? 1.0f : it->second;
    }
    /// : gate de condiciones — vive en el CORE (el mismo evaluador que
    /// usan los tiles). NULL cuando el pack no trae ninguna condicion, que es
    /// el caso normal: asi no se consulta nada por frame.
    struct GateDel { void operator()(AudioEventGate* g) const { ayther_audio_gate_free(g); } };
    std::unique_ptr<AudioEventGate, GateDel> audio_gate;
    /// Firmas que este frame quedaron BLOQUEADAS por sus condiciones: suenan
    /// en original. Se recalcula una vez por frame, no por disparo.
    std::unordered_set<uint64_t> audio_gate_blocked;
    bool audio_gated(uint64_t sig) const {
        return !audio_gate_blocked.empty() && audio_gate_blocked.count(sig) != 0;
    }

    /// : region de loop AUTORADA por firma, en cuadros del asset.
    /// Ausente = (0,0) = el asset entero (contrato de siempre).
    std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> audio_event_loop;
    std::pair<uint32_t, uint32_t> loop_of(uint64_t sig) const {
        const auto it = audio_event_loop.find(sig);
        return it == audio_event_loop.end() ? std::make_pair(0u, 0u) : it->second;
    }
    uint32_t tail_of(uint64_t sig) const {
        const auto it = audio_event_tail.find(sig);
        return it == audio_event_tail.end() ? kTailUnlimited : it->second;
    }
    /// : frame de CORTE absoluto de una ventana [.., end] según su tail
    /// (UINT64_MAX = drena entero). El MISMO número gobierna al player
    /// (EventStream.cut_frame), al barrido de one-shots y al export.
    uint64_t cut_frame_of(uint64_t sig, uint64_t end_frame) const {
        const uint32_t t = tail_of(sig);
        return t == kTailUnlimited ? UINT64_MAX : end_frame + t;
    }
    /// One-shots per-firma con tail FINITO en el aire: key → frame de corte.
    /// produce_frame los barre y corta con el fade rápido del player.
    std::unordered_map<uint64_t, uint64_t> hd_oneshot_cut;
    // : firmas MIEMBRO por Secuencia (audio_events.toml `members`) — con
    // esto la ventana mutea SOLO los eventos activos de estas firmas; sin
    // members (packs viejos) cae al range-mute de `channels`.
    std::unordered_map<uint64_t, std::vector<uint64_t>> audio_event_members;
    /// : CABEZA por firma disparadora (`head` del TOML) + paso del próximo
    /// anclaje por firma (segmentación: una ocurrencia interna no re-ancla).
    std::unordered_map<uint64_t, std::vector<uint64_t>> audio_event_head;
    std::unordered_map<uint64_t, uint64_t>              audio_event_seq_next;
    /// : las entradas de SECUENCIA del catálogo como las ve el anclaje.
    std::vector<SeqAnchorSub> audio_event_seq_view() const {
        std::vector<SeqAnchorSub> v;
        for (const auto& [sig, dur] : audio_event_duration) {
            if (!dur) continue;
            SeqAnchorSub a;
            a.key = sig; a.trigger_signature = sig; a.duration_frames = dur;
            const auto as = audio_event_assign.find(sig);
            a.enabled = as != audio_event_assign.end() && !as->second.empty();
            const auto lp = audio_event_looping.find(sig);
            a.looping = lp != audio_event_looping.end() && lp->second;
            const auto ms = audio_event_members.find(sig);
            if (ms != audio_event_members.end()) a.signatures = ms->second;
            const auto hd = audio_event_head.find(sig);
            if (hd != audio_event_head.end()) a.head = hd->second;
            v.push_back(std::move(a));
        }
        std::sort(v.begin(), v.end(), [](const SeqAnchorSub& x, const SeqAnchorSub& y) {
            return x.key < y.key;
        });
        return v;
    }
    //  F3: regla de match por asignación (opt-in; ausente = exacta legacy)
    // + índice instrumento→asignaciones para resolver una voz en O(1). El
    // índice se RECONSTRUYE al mutar asignaciones/reglas (nunca por frame).
    std::unordered_map<uint64_t, AudioMatchRuleInfo> audio_event_rule;
    AudioMatchIndex audio_match_index;

    // ---- E-2 (): suscripciones de la ABI AYTHER v1 ---------------------
    // El fork compila con el perfil ESTÁNDAR: ningún subsistema de observación
    // trabaja hasta que el frontend declare qué necesita. Los accesos legacy
    // (0x100-0x10E) funcionaban salteándose ese sistema — escribían la memoria
    // del core directo—, así que con la ABI hay que pedir explícitamente.
    //
    // Se pide `AYTHER_SUB_ALL & supported_mask` y no `ALL` a secas: el core
    // puede estar compilado sin algún subsistema, y pedir lo que no existe
    // haría fallar la llamada entera en vez de degradar.
    uint32_t ayther_subs_requested = 0;
    bool     ayther_subs_verified  = false;
    // : telemetría del juez de framebuffer (última pasada de scene_inventory).
    mutable uint32_t judge_occs = 0, judge_dropped = 0, judge_opaque = 0, judge_hits = 0;

    /// Pide las suscripciones. No-op con un core sin ABI (camino legacy).
    void activate_ayther_subscriptions() {
        if (!runner.has_ayther_v1()) return;
        const ayther_interface_v1* api = runner.ayther_api();
        if (!(api->capabilities & AYTHER_CAP_SUBSCRIPTIONS_V1)) return;
        ayther_subscription_state_v1 st{};
        st.struct_size = sizeof(st);
        if (api->get_subscriptions(&st, sizeof(st)) != AYTHER_STATUS_OK) {
            std::fprintf(stderr,
                "[AytherSession] get_subscriptions fallo — sin suscripciones\n");
            return;
        }
        // Sólo lo que el Engine LEE (ABI 1.9, guía §4). AYTHER_SUB_ALL pasó
        // de 0x7F a 0xFFF y los bits nuevos cuestan por frame sin que nadie
        // los consuma todavía — ver RetroRunner::kEngineSubscriptions.
        const uint32_t want = RetroRunner::kEngineSubscriptions & st.supported_mask;
        const int32_t  rc   = api->set_subscriptions(want);
        if (rc != AYTHER_STATUS_OK) {
            std::fprintf(stderr,
                "[AytherSession] set_subscriptions fallo: %d\n", rc);
            return;
        }
        ayther_subs_requested = want;
        ayther_subs_verified  = false;   // se confirma tras el primer frame
        std::fprintf(stdout,
            "[AytherSession] suscripciones AYTHER pedidas: 0x%08X "
            "(soportadas: 0x%08X)\n",
            want, st.supported_mask);
    }

    // ---- E-3 (): el ESPEJO por frame de la ABI -------------------------
    // Los callers del camino por frame leían punteros VIVOS del core
    // (`video_ram()`, `color_ram()`…). Con la ABI la lectura es una copia
    // validada contra la generación del snapshot, así que el dual-path vive
    // ACÁ, en un solo lugar, y no repartido en los ~25 sitios que consumen esos
    // punteros: cada uno de esos sitios es una oportunidad de equivocarse, y el
    // objetivo de E-3 —que los bytes vengan por la ABI— se cumple igual.
    //
    // El espejo se refresca UNA vez por frame, después de `run_frame`, que es
    // cuando la ABI ya cerró su frame boundary. Sin ABI queda vacío y los
    // helpers devuelven el puntero legacy de siempre.
    ayther_frame_snapshot_v1 abi_snap{};
    bool                     abi_snap_ok = false;
    // ABI 1.5 `SYSTEM`: modo del VDP y viewport del contenido cargado, leído
    // una vez al crear la sesión (guía 1.9 §5.1). `sys_ok` = el core lo dio;
    // sin él (stock, fork viejo) se decodifican registros como siempre.
    ayther_system_v1         sys{};
    bool                     sys_ok = false;
    bool                     sys_logged = false;   ///< el primer frame con modo, una vez
    // ABI 1.9 §5.8: los dos motivos de fallback que merecen aviso propio, una
    // vez por sesión — el resto de la máscara sigue siendo «fallback» a secas.
    bool                     raster_overflow_logged  = false;
    bool                     raster_unsupported_logged = false;
    std::vector<uint8_t>     abi_vram, abi_cram, abi_regs, abi_vsram;
    std::vector<ayther_sprite_v1>      abi_sprites;
    // Cuántas entradas de `abi_sprites` son VÁLIDAS. El vector se dimensiona
    // con el count del snapshot, pero la lectura devuelve el suyo y puede ser
    // menor: usar size() como cantidad publica la cola sin llenar como si
    // fueran sprites reales (y el viewport los dibuja con patrones basura).
    // 0 = el espejo no tiene nada que ofrecer; el caller cae al legacy.
    uint32_t                           abi_sprite_count = 0;
    std::vector<ayther_audio_write_v1> abi_audio;
    bool abi_sprites_warned = false, abi_audio_warned = false;

    // Escape hatch de diagnóstico: AYTHER_ABI_MIRROR=0 apaga el espejo y deja
    // todo el dual-path cayendo al legacy. Sirve para aislar en UNA corrida si
    // una diferencia visual/de detección viene del espejo o de otro lado, con
    // el MISMO binario a los dos lados del A/B.
    static bool mirror_enabled() {
        static const bool on = [] {
            const char* v = ayther::env_get("AYTHER_ABI_MIRROR");
            return !(v && v[0] == '0');
        }();
        return on;
    }

    void refresh_abi_mirror() {
        abi_snap_ok = false;
        abi_sprites.clear();
        abi_sprite_count = 0;
        abi_audio.clear();
        if (!mirror_enabled()) return;
        if (!runner.has_ayther_v1()) return;
        if (!runner.capture_frame_snapshot(abi_snap).ok()) return;
        // SYSTEM se refresca POR FRAME, no una vez: al crear la sesión el VDP
        // todavía no eligió modo (`vdp_mode == 0`, viewport por defecto) y
        // h40/interlace cambian con el juego. Es una lectura chica y sin
        // suscripción — se llena al leer.
        sys_ok = runner.read_system_v1(sys).ok();
        if (sys_ok && !sys_logged && sys.vdp_mode != 0) {
            sys_logged = true;
            std::fprintf(stdout,
                "[AytherSession] SYSTEM: hw=0x%02X vdp_mode=%u h40=%u interlace=%u "
                "sh=%u %s lines=%u viewport=%ux%u@(%u,%u) geometry_pending=%u\n",
                sys.system_hw, sys.vdp_mode, sys.h40, sys.interlace,
                sys.shadow_highlight, sys.region_pal ? "PAL" : "NTSC",
                sys.lines_per_frame, sys.viewport_w, sys.viewport_h,
                sys.viewport_x, sys.viewport_y,
                (unsigned)(sys.flags & AYTHER_SYSTEM_GEOMETRY_PENDING));
        }
        // El buffer se dimensiona por el MAYOR de los dos tamaños declarados:
        // `read_*_v1` escribe los bytes que dice la ABI (`query_region`), no los
        // que dice `retro_get_memory_size`. Hoy coinciden, pero dimensionar por
        // el número legacy era apostar a que sigan coincidiendo — y esa apuesta
        // se paga con un desbordamiento de heap, no con un dato raro.
        auto read_region = [&](std::vector<uint8_t>& dst, size_t n_legacy, uint32_t region,
                        RetroRunner::AytherReadResult (RetroRunner::*fn)(
                            void*, const ayther_frame_snapshot_v1&) const) {
            const size_t n_abi = runner.abi_region_bytes(region);
            const size_t n     = n_abi > n_legacy ? n_abi : n_legacy;
            if (!n) { dst.clear(); return; }
            dst.resize(n);
            const auto r = (runner.*fn)(dst.data(), abi_snap);
            // Y se publica lo LEÍDO, no lo pedido: una lectura corta dejaría la
            // cola sin llenar viajando como si fuera memoria del core (es el
            // defecto que ya mordió en los sprites del viewport, ).
            // Truncar NO alcanza: los accessors públicos (`vdp_regs(&size)` y
            // compañía) devuelven el puntero del espejo con el tamaño LEGACY,
            // así que un espejo más corto se leería de más. Una lectura corta
            // lo vuelve inservible → se cae al legacy, que sí mide lo que dice.
            if (!r.ok() || (r.count && r.count < n)) dst.clear();
        };
        read_region(abi_vram,  runner.video_ram_size(), AYTHER_REGION_VRAM,     &RetroRunner::read_vram_v1);
        read_region(abi_cram,  runner.color_ram_size(), AYTHER_REGION_CRAM,     &RetroRunner::read_cram_v1);
        read_region(abi_regs,  runner.vdp_regs_size(),  AYTHER_REGION_VDP_REGS, &RetroRunner::read_vdp_regs_v1);
        read_region(abi_vsram, runner.vsram_size(),     AYTHER_REGION_VSRAM,    &RetroRunner::read_vsram_v1);
        abi_snap_ok = true;
    }

    // Los helpers del dual-path. Si una región del espejo quedó vacía (no
    // suscripta, o el core la rechazó) se cae al puntero legacy en vez de
    // devolver nullptr: degradar a lo que funcionaba es mejor que apagar la
    // detección de sprites porque una lectura falló.
    AYTHER_LEGACY_READ_BEGIN
    const uint8_t* vram_ptr()  const {
        return !abi_vram.empty()  ? abi_vram.data()  : runner.video_ram();
    }
    const uint8_t* cram_ptr()  const {
        return !abi_cram.empty()  ? abi_cram.data()  : runner.color_ram();
    }
    const uint8_t* regs_ptr()  const {
        return !abi_regs.empty()  ? abi_regs.data()  : runner.vdp_regs();
    }
    const uint8_t* vsram_ptr() const {
        return !abi_vsram.empty() ? abi_vsram.data() : runner.vsram();
    }
    AYTHER_LEGACY_READ_END

    /// Confirma —UNA vez, tras el primer frame— que el core las activó. Es
    /// diagnóstico y no bloquea: las suscripciones entran en el frame boundary,
    /// así que preguntarlo antes de correr un frame siempre daría 0.
    void verify_ayther_subscriptions() {
        if (ayther_subs_verified || !ayther_subs_requested) return;
        if (!runner.has_ayther_v1()) return;
        const ayther_interface_v1* api = runner.ayther_api();
        if (!(api->capabilities & AYTHER_CAP_SUBSCRIPTIONS_V1)) return;
        ayther_subscription_state_v1 st{};
        st.struct_size = sizeof(st);
        if (api->get_subscriptions(&st, sizeof(st)) != AYTHER_STATUS_OK) return;
        if (st.active_mask == ayther_subs_requested)
            std::fprintf(stdout,
                "[AytherSession] suscripciones AYTHER activas: 0x%08X\n",
                st.active_mask);
        else
            std::fprintf(stderr,
                "[AytherSession] suscripciones DESALINEADAS — activas=0x%08X "
                "pedidas=0x%08X\n", st.active_mask, ayther_subs_requested);
        ayther_subs_verified = true;
    }
    // ---- E-7 (): capas del VDP ----------------------------------------
    // Los cinco buffers viven acá y se reusan: a 320x224 son 5 x 143 KB, y
    // realocarlos por frame seria pagar un malloc por capa a 60 Hz. Orden:
    // 0=B, 1=A, 2=ventana, 3=sprites, 4=composite.
    std::vector<uint16_t> layer_bufs[5];
    void*   multilayer_fn = nullptr;        ///< el export, resuelto una vez
    bool    multilayer_fn_resolved = false;
    int32_t layers_error_status = AYTHER_STATUS_OK;
    /// El ultimo motivo YA LOGUEADO, para no repetirlo frame a frame. Distinto
    /// de `layers_error_status`, que es el de la ultima llamada: si el juego
    /// entra y sale del modo 5, cada transicion vuelve a loguear una vez.
    int32_t layers_error_logged = AYTHER_STATUS_OK;

    /// : los Acetatos que trajo el pack. La sesión los lee y los ofrece —
    /// el stack lo arma el frontend (ver pack_overlays() en el header).
    std::vector<AytherSession::PackOverlay> overlays;
    void rebuild_match_index() {
        audio_match_index.clear();
        for (const auto& [sig, r] : audio_event_rule)
            if (audio_event_assign.count(sig))
                audio_match_index.add(sig, r.rule, r.instrument, r.pitch);
    }
    ///  F3: la firma AUTORADA que cubre una voz — la exacta si está
    /// asignada, o la variante por regla (instrument == 0 nunca matchea).
    /// `sig` sigue siendo la ocurrencia real (flancos/keys); `*out` es la
    /// entrada del catálogo (asset/ventana/tail/readiness).
    bool resolve_event_sig(uint64_t sig, uint64_t instrument, uint8_t pitch,
                           uint64_t* out) const {
        if (audio_event_assign.count(sig)) {
            if (out) *out = sig;
            return true;
        }
        return audio_match_index.resolve(instrument, pitch, out);
    }
    struct SeqWindow { uint64_t end; uint32_t mask; uint64_t sig; uint64_t start = 0; };
    std::vector<SeqWindow> audio_seq_windows;
    /// : anclas de TODAS las subs de Secuencia, calculadas en UNA pasada
    /// conjunta sobre audio_events (ver seq_anchor_table). Cache por (cantidad
    /// de eventos, generación de subs): el set de subs se re-manda cada frame
    /// pero sólo cambia de generación cuando cambia de verdad.
    std::unordered_map<uint64_t, std::vector<uint32_t>> seq_anchor_cache;
    size_t   seq_anchor_for_n   = SIZE_MAX;
    uint64_t seq_anchor_for_gen = UINT64_MAX;
    uint64_t audio_seq_subs_gen = 0;
    // EM-1: cámara en espacio de NIVEL — unwrap secuencial del scroll por
    // plano (A/B). Discontinuidad de frame (seek/scrub/catch-up) re-ancla.
    int32_t  cam_x[2] = {0, 0}, cam_y[2] = {0, 0};
    int16_t  cam_prev_h[2] = {0, 0}, cam_prev_v[2] = {0, 0};
    uint64_t cam_last_frame = UINT64_MAX;
    bool     cam_valid = false;
    bool                              audio_sub_preview = false;
    // Estado del TRANSPORTE (lo setea la app por frame): los HD asignados solo
    // DISPARAN reproduciendo — al scrubear con el cabezal quieto sonaban a
    // velocidad normal, se superponían (eco) y seguían tras detenerse
    // (reporte 2026-07-23). Pausar corta lo que está en el aire.
    bool                              transport_playing = true;
    // Salida AUDIBLE (la app, por frame): false = el produce descarta su PCM
    // (cargar tomas/poses hace seeks/re-produces que no deben sonar). true por
    // default — runtime/Play y los tools no lo tocan.
    bool                              audio_audible     = true;
    /// Contadores de por qué NO sonó un frame (ver el flush en step()). Cuatro
    /// caminos que desde afuera se ven idénticos —silencio— y tienen causas y
    /// arreglos distintos. Los expone audio_health.
    uint64_t aud_n_flushed = 0, aud_n_inaudible = 0,
             aud_n_quiet   = 0, aud_n_disabled  = 0;
    // Sustitución EN VIVO (runtime, C-A4 paso 3): detector alimentado por frame +
    // máscara/firmas activas del frame anterior (1 frame de lag inherente: el
    // key-on de este frame se detecta recién tras run_frame).
    AudioEventPtr                     audio_live_det;
    bool                              audio_runtime_sub = false;
    uint32_t                          audio_runtime_mask = 0;
    std::unordered_set<uint64_t>      audio_live_prev;   // firmas activas el frame anterior
    // SECUENCIAS de autoría EN VIVO (Capturar): ventana abierta por el key-on
    // real de la firma disparadora de una sub (audio_seq_subs) sobre el
    // detector — mismo modelo que el pack exportado (range-mute de la unión de
    // canales + HD one-shot) pero con los datos de autoría (asset de disco,
    // gain). Sólo con audio_runtime_sub activo.
    struct LiveSeqWin {
        uint64_t key       = 0;   ///< id de la Secuencia (la key de su sub)
        uint64_t end_frame = 0;   ///< fin del range-mute (inclusive)
        uint64_t start_frame = 0; ///< : ancla (para el reclamo/continuación)
        uint32_t mask      = 0;   ///< canales muteados mientras la ventana vive
        uint64_t last_seen = 0;   ///< último frame con un evento miembro activo
    };
    std::vector<LiveSeqWin>                audio_live_seq_win;
    std::unordered_map<uint64_t, uint64_t> audio_live_seq_next;  // key → frame mínimo del próximo anclaje (paso = span)
    // : instancia LÓGICA por reemplazo live — separa «el evento sigue
    // activo» (esto) de «el stream está sonando» (AudioPlayer). La pausa
    // () y el bypass de Assets destruyen/omiten los streams pero
    // conservan estas instancias; reanudar las vuelve a sonar DESDE EL
    // OFFSET del reloj emulado (audio_live_resume.h) — sin esperar un
    // key-on nuevo, sin reiniciar desde cero y SIN limpiar audio_live_prev
    // (limpiarlo corrige el silencio pero mete desfase y dobles disparos).
    struct LiveInstance {
        std::string asset;                      ///< path (pack o disco)
        uint64_t    start_frame = 0;            ///< anclaje (rising-edge real)
        uint64_t    end_frame   = UINT64_MAX;   ///< fin de ventana; MAX = one-shot libre
        uint64_t    cut_frame   = UINT64_MAX;   ///< end + tail ()
        uint32_t    ev_bit      = 0;            ///< canal del evento (mute al reanudar)
        float       gain        = 1.0f;         ///< slider de la Secuencia
        bool        looping     = false;
        bool        seq_sub     = false;        ///< key = Secuencia de autoría (Capturar)
    };
    std::unordered_map<uint64_t, LiveInstance> audio_live_inst;
    /// : telemetría de la reanudación — streams re-armados con offset,
    /// instancias vencidas descartadas, y cuadros de offset acumulados
    /// (cada uno es un reinicio-desde-cero evitado).
    uint64_t hd_resumed = 0, hd_resume_finished = 0, hd_resume_offset_frames = 0;
    ///  Fase 3: Assets OFF en un workspace vivo = BYPASS — el detector y
    /// el bookkeeping (ventanas/instancias/flancos) siguen corriendo para no
    /// perder el hilo de los eventos, pero la máscara queda en 0 (suena el
    /// original) y ningún HD dispara. Volver a ON re-entra por el MISMO
    /// camino que reanudar una pausa: offset del reloj emulado.
    bool audio_live_bypass = false;
    //  Fase 4: OBSERVABILIDAD del match live. El síntoma reportado
    // (2026-08-10, transición pantalla→demo de Amazona) es «suenan originales
    // además de los assets» y desde afuera no se puede saber POR QUÉ: si son
    // colas de la escena anterior o firmas fragmentadas por el estado del
    // chip en la transición. Esto lo vuelve dato: cada firma ACTIVA se
    // clasifica por frame en exacta (asignada), variante (mismo instrumento
    // que una asignada — la fragmentación de ) o sin match, y las sin
    // match se acumulan en un registro acotado con su historia.
    //
    // El instrumento se APRENDE de los eventos CERRADOS del detector — desde
    //  F3 el FFI de actives TAMBIÉN lo trae (al key-on); lo aprendido
    // queda como memoria (armar reglas sobre firmas que ya no suenan) y
    // fallback. La nota se guarda junto al timbre (reglas kInstrumentPitch).
    struct LiveSigId { uint64_t instrument = 0; uint8_t pitch = kAudioNoPitch; };
    std::unordered_map<uint64_t, LiveSigId> live_sig_instr;    // sig → identidad
    std::unordered_set<uint64_t>            live_assigned_instr; // instr de asignadas
    struct LiveUnmatchedRec {
        uint64_t instrument   = 0;   ///< 0 = aún desconocido
        uint64_t first_frame  = 0;
        uint64_t frames_active = 0;
        uint8_t  chip = 0, channel = 0;
        bool     variant = false;    ///< mismo instrumento que una asignada
    };
    std::unordered_map<uint64_t, LiveUnmatchedRec> live_unmatched;
    static constexpr size_t kLiveUnmatchedCap = 128;
    //  F3: `live_match_rule` = frame-ocurrencias resueltas por una REGLA
    // de match (fragmentación que la regla cubrió — antes contaban `variant`).
    uint64_t live_match_exact = 0, live_match_rule = 0,
             live_match_variant = 0, live_match_none = 0;
    std::vector<AytherAudioEvent> live_evt_scratch;   // lectura de cerrados
    /// Firmas ACTIVAS del último frame que procesó el detector live. La
    /// política del router decide sus voces con esto: en vivo no hay toma
    /// alineada que consultar, y el detector corre ANTES del voice_tick en el
    /// mismo produce — el key-on que el router está decidiendo ya está acá.
    std::vector<AytherAudioActive> live_active;

    /// ¿Esta firma está CUBIERTA por la autoría? (asignación por evento, o
    /// disparadora/miembro de una Secuencia con asset)
    bool live_sig_covered(uint64_t sig) const {
        if (audio_event_assign.count(sig)) return true;
        for (const auto& sq : audio_seq_subs) {
            if (sq.asset.empty()) continue;
            if (sq.trigger_signature == sig) return true;
            if (std::find(sq.signatures.begin(), sq.signatures.end(), sig)
                    != sq.signatures.end()) return true;
        }
        return false;
    }
    uint32_t                          audio_manual_mute = 0;   // mute por canal a mano (timeline Audios)
    std::unordered_set<uint64_t>      audio_instrument_mute;   // mute DINÁMICO por instrument (panel Sonidos)
    // Mute por OCURRENCIA exacta (clave chip<<56|canal<<48|start): Secuencias
    // deshabilitadas con el ojo — ni el HD (su sub se excluye) ni el sonido
    // ORIGINAL deben oírse (reporte 2026-07-23).
    std::unordered_set<uint64_t>      audio_occurrence_mute;

    // -- RE-SÍNTESIS CON SOUNDFONT () ------------------------------------
    // Un timbre del juego (el `instrument` del detector) se reemplaza por un
    // preset de SoundFont. Es un eje COMPLEMENTARIO a la Secuencia: el juego
    // SIGUE tocando —su tempo, sus cortes— y sólo cambia el TIMBRE de una voz,
    // así que no puede desincronizar. Escalera: Secuencia > Instrumento.
    struct InstAssign {
        std::string soundfont;   ///< basename; el pack lo trae recortado
        uint16_t    bank = 0, preset = 0;
        int8_t      transpose = 0;
        float       gain = 1.0f;
    };
    std::unordered_map<uint64_t, InstAssign> inst_assign;      // instrument → preset
    /// : lo mismo, pero HORNEADO — sale de `instruments.toml` del pack en
    /// `load_pack_into`. Se mantiene aparte por el mismo criterio que
    /// `element_enhance_lab` / `element_enhance_pack` (): lo que el
    /// frontend está autorando manda sobre lo horneado, así que el del pack
    /// sólo se aplica cuando NADIE autoró (que es el caso de Play y del
    /// runtime, donde no hay quien mande el catálogo cada frame).
    std::unordered_map<uint64_t, InstAssign> inst_assign_pack;
    /// Un sintetizador POR ARCHIVO de SoundFont: cada uno se hornea recortado
    /// por su cuenta, y así un proyecto puede mezclar timbres de varios.
    /// Un sintetizador POR TIMBRE, no por archivo (). Hace falta para poder
    /// REALZAR la ganancia: CC 7 se acaba en 127, así que subir exige escalar el
    /// buffer, y eso sólo es correcto si el sintetizador atiende a una sola voz.
    /// El SoundFont parseado se comparte del lado Rust, así que N timbres del
    /// mismo archivo no lo duplican.
    std::unordered_map<uint64_t, AytherSf2*> synths;   // instrument → synth
    /// Factor de REALCE por timbre (>= 1.0). La atenuación va por CC 7; el
    /// realce, escalando el render de ESE sintetizador.
    std::unordered_map<uint64_t, float> synth_boost;
    /// Canal MIDI asignado a cada instrument. El sintetizador tiene 16 y el
    /// juego a lo sumo 10 voces, así que entran todas sin robarse notas — que
    /// es lo que pasaría mandando todo al canal 0.
    /// Notas en vuelo, para poder cerrarlas. `opened` es el frame en que
    /// arrancó: hace falta para el piso de duración (ver kSynthMinHold).
    /// `occ` es la clave de OCURRENCIA del evento que la abrió: hace falta para
    /// poder cerrarla si el artista silencia esa ocurrencia (o su canal) con la
    /// nota ya sonando — el instrumento solo no alcanza ().
    struct SynthNote { uint64_t inst, occ; int ch, key; int64_t opened, ends; bool one_shot; };
    std::vector<SynthNote> synth_on;

    /// Timbres que se disparan como ONE-SHOT (percusivos), cacheado.
    /// Se decide POR TIMBRE y no por evento: ver la nota de `synth_one_shot`.
    mutable std::unordered_map<uint64_t, bool> synth_oneshot_cache;
    mutable size_t synth_oneshot_for_n = 0;   // invalida si cambió el análisis

    /// ¿Este timbre se dispara y se deja sonar, o se sostiene y se suelta?
    ///
    /// LA DECISIÓN ES POR TIMBRE, NO POR EVENTO, y eso costó una escucha. S15
    /// de Demo Barbaro —el bombo— tiene eventos de 1 a 27 frames: con un umbral
    /// por evento, el MISMO golpe recibía dos tratamientos y sonaba «a veces
    /// piano, otros piano interrumpido». El juego suena consistente porque en
    /// FM el key-off arranca un release rápido y da casi igual cuánto se
    /// sostuvo; traducirlo a «nota de piano de 450 ms» contra «nota libre» no.
    ///
    /// El criterio: un timbre que SIEMPRE toca la misma nota es percusión —un
    /// bombo, un hi-hat, un golpe— y va como one-shot. Uno que recorre alturas
    /// es melódico y ahí sostener y soltar SÍ es parte de la interpretación.
    /// Sale de los datos, no de un campo que el artista tenga que completar.
    bool synth_one_shot(uint64_t inst) const {
        if (synth_oneshot_for_n != audio_events.size()) {
            synth_oneshot_cache.clear();
            synth_oneshot_for_n = audio_events.size();
        }
        auto it = synth_oneshot_cache.find(inst);
        if (it != synth_oneshot_cache.end()) return it->second;
        uint8_t lo = 255, hi = 0;
        for (const AytherAudioEvent& e : audio_events) {
            if (e.instrument != inst || e.pitch == 255) continue;
            if (e.pitch < lo) lo = e.pitch;
            if (e.pitch > hi) hi = e.pitch;
        }
        const bool one = (hi >= lo) && (lo == hi);
        synth_oneshot_cache[inst] = one;
        return one;
    }

    /// (Legado del umbral por evento — ya no se usa para decidir; queda el
    /// número por si hiciera falta un piso para timbres melódicos muy cortos.)
    ///
    /// Por qué. Muchos eventos del Mega Drive duran UN frame (un bombo, un
    /// golpe). Cerrarlos a los 16 ms mete el note_off dentro del ataque y no se
    /// oye nada; cerrarlos a un piso fijo los corta a mitad de la cola y suena
    /// «interrumpido». Las dos cosas se reportaron. Un piso fijo es un término
    /// medio malo: largo para el re-disparo rápido, corto para la cola.
    ///
    /// El chip tampoco cierra nada: su key-off arranca un release que sigue
    /// sonando. Un evento LARGO sí se cierra —ahí el juego sostiene la nota y
    /// soltarla es parte de la interpretación—, pero uno corto es un impacto y
    /// se deja sonar.
    ///
    /// El umbral es una heurística a ajustar de oído, no una medida.
    static constexpr int64_t kSynthOneShotMax = 4;
    std::vector<float> synth_pcm;   // buffer de render, reusado
    /// Muestras que el emulador stageó ESTE frame, capturadas ANTES del flush
    /// (que las descarta). Es la medida correcta de «cuánto audio vale este
    /// frame»; leerla después del flush daba 0 y el sintetizador caía a un
    /// número fijo que lo mataba de hambre.
    size_t  synth_frames_hint = 0;
    bool    synth_pcm_ready = false;   // synth_pcm listo para que lo sume el router
    bool  synth_any = false;        // hay al menos una asignación viva
    int64_t synth_last_frame = -1;  // último frame en que se avanzó

    /// Telemetría del sintetizador (). «Se escucha degradado, o no se
    /// escucha, y pocas veces bien» se ve IGUAL desde afuera venga de un corte
    /// por salto (seek/catch-up: apaga todas las notas), de un note_on que
    /// nunca ocurrió, de un timbre silenciado o de un frame sin PCM del
    /// emulador — y cada causa tiene un arreglo distinto. Diagnosticar eso de
    /// oído ya costó varias vueltas.
    uint64_t syn_ticks = 0,   // frames de síntesis efectivamente avanzados
             syn_jumps = 0,   // cortes por salto de frame (panic)
             syn_on    = 0,   // note_on emitidos
             syn_off   = 0,   // note_off emitidos
             syn_muted = 0,   // notas NO disparadas por timbre silenciado
             syn_nopcm = 0;   // frames sin PCM del emulador (largo estimado)

    // -- Router de canales por voz () ------------------------------------
    // PUESTO por defecto (2026-07-28): lo prende `create` llamando al setter, que
    // es donde vive la inicialización real (política, PAL, tasas del resampler y
    // el cebado del espejo). Por eso el miembro arranca en false y NO hay que
    // «corregirlo» a true acá: hacerlo saltearía todo eso — el setter no hace
    // nada si el valor ya coincide.
    bool                  voice_router_on = false;
    /// : el router SUMA su bloque al del chip en vez de ocupar su lugar.
    /// Pasa en el único hardware donde el buffer del core lleva audio que el
    /// router no espeja —el PCM RF5C164 y el CDDA del Sega CD—; ahí ocupar el
    /// lugar dejaba el sistema mudo. En cartucho el camino no cambia en nada.
    bool router_mix() const { return voice_router_on && runner.cd_media(); }
    ChannelRouter         voice_router;
    StreamResampler       voice_rs;
    int64_t               voice_last_frame = -1;
    /// Los frames cuyas escrituras todavía no consumió el router, EN ORDEN.
    /// Incluye los frames bare del catch-up: sus escrituras existen (el juego
    /// las hizo) pero produce_frame resetea el log al empezar, así que si no se
    /// capturan ahí se pierden — y el espejo se quedaría sin los key-on y los
    /// cambios de patch de hasta 31 frames.
    /// Cada entrada lleva SU frame. Con el catch-up se rinden varios frames en
    /// un tick, y la política decide por evento —que está indexado por frame de
    /// key-on—, así que pasarles a todos el frame actual elegiría mal.
    struct PendingFrame { uint32_t frame; std::vector<AytherAudioWrite> writes; };
    std::vector<PendingFrame> voice_pending;
    std::vector<float>    voice_chip;   // salida del router, a la tasa del chip
    std::vector<float>    voice_out;    // ya resampleada, a la del device
    uint64_t vr_ticks = 0, vr_primes = 0, vr_starved = 0, vr_frames = 0,
             vr_skipped = 0,   // frames cebados sin rendir (no iban a sonar)
             vr_resyncs = 0;   // veces que hubo que tirar atraso acumulado
    /// Diagnóstico del atraso. Si el router rinde MÁS audio del que el device
    /// consume, la diferencia se queda en el resampler como latencia permanente
    /// — se oiría desfasado de la imagen, no entrecortado, y por eso hay que
    /// medirlo y no deducirlo.
    size_t   vr_backlog = 0, vr_pend_max = 0;
    /// Tee de lo que el router entrega al device (AYTHER_VOICE_DUMP=<ruta>).
    /// El tee que ya existía (AYTHER_AUDIO_DUMP) saca el PCM del EMULADOR, que
    /// con el router puesto es silencio — no sirve para oír lo que se oye.
    /// Crudo f32 estéreo: sin cabecera que parchear al cerrar, así se puede
    /// levantar el archivo con el Lab todavía abierto.
    FILE*    voice_dump = nullptr;
    /// Tee del bloque del SoundFont SOLO (AYTHER_SF2_DUMP=<ruta>). Separa «no
    /// suena» de «suena bajo» de «suena y se corta» sin depender del oído, que
    /// es lo que resolvió las tres cacerías anteriores.
    FILE*    sf2_dump = nullptr;
    /// Colchón del stream del router, en muestras a 44,1 kHz. 250 ms cubre con
    /// margen los tirones de 90-150 ms que mide el probe de frame.
    static constexpr size_t kVoiceCushionFrames = 11025;   // 250 ms
    size_t   vr_queued = 0;   // lo que le queda al stream sin consumir
    double   vr_sf2_rms = 0.0, vr_sf2_peak = 0.0;

    // -- Fallback transaccional () ---------------------------------------
    /// Keys (firma de evento / key de Secuencia) cuyo ÚLTIMO intento de
    /// arranque HD falló con el asset ya decodificado (stream/bind SDL): sus
    /// ventanas dejan de silenciar el original hasta que un disparo posterior
    /// funcione. Los fallos de ASSET (missing/corrupt/etc.) no viven acá — los
    /// responde hd_ready por la cache del player, con reintento por fingerprint.
    std::unordered_set<uint64_t> hd_failed_keys;
    /// : assets distintos que fallaron, por subsistema. La clave es el
    /// índice de `Subsystem`; el valor, las RUTAS — no las firmas, porque un
    /// mismo archivo roto asignado a doce eventos es un archivo roto y no doce.
    FailureEscalation escalation;
    /// Subsistemas que el MOTOR apagó solo. Aparte de `subsystems_on` a
    /// propósito: hay que poder distinguir «el usuario lo apagó» de «se apagó
    /// por fallos», porque sólo del segundo hay algo que contarle al usuario.
    uint32_t auto_disabled_on = 0;
    /// Ocurrencias donde sonó el ORIGINAL porque el HD asignado no pudo
    /// (asset no listo o arranque fallido). El observable del fallback: crece
    /// y se oye el juego = la regla funciona; crece y hay silencio = hay un
    /// mute fuera del handshake.
    uint64_t hd_fallback = 0;

    /// ¿El asset HD de una asignación puede sonar AHORA? Decodificado y listo,
    /// del lado que le toque (pack en runtime, disco en autoría) — la MISMA
    /// elección que hace el disparo. Asignado ≠ reproducible: esta pregunta es
    /// la que autoriza a silenciar el original.
    bool hd_ready(const std::string& asset) {
        if (asset.empty()) return false;
        if (pack && ayther_pack_file_size(pack.get(), asset.c_str()) > 0)
            return audio.asset_ready_pack(pack.get(), asset);
        return audio.asset_ready_disk(asset);
    }
    /// hd_ready + sin un fallo de arranque pendiente para esta key.
    ///
    /// : el routing de audio entra POR ACÁ y no en el disparo, y esa
    /// elección es la que lo hace correcto: ésta es la pregunta ÚNICA que
    /// decide las dos mitades () — si el HD no puede sonar, el original
    /// tampoco calla. Apagar el audio HD devuelve el sonido original sin tocar
    /// ningún otro camino y sin dejar un hueco de silencio, que es exactamente
    /// lo que «restauración inmediata del contenido original» quiere decir.
    ///
    /// Música y Efectos todavía no se distinguen —eso llega con el «Tipo» de la
    /// Secuencia ()—, así que el gate es el PAR: con los dos apagados no
    /// hay audio HD. Con uno solo apagado no se puede decidir de qué bus es
    /// este sonido, y apagar el que no era sería peor que no apagar nada.
    bool hd_can_sound(uint64_t key, const std::string& asset) {
        if (!sub_on(Subsystem::Music) && !sub_on(Subsystem::Sfx)) return false;
        if (hd_failed_keys.count(key)) return false;
        if (hd_ready(asset)) return true;
        note_asset_failure(key, asset);
        return false;
    }

    // -- : ESCALADA tras fallos repetidos --------------------------------
    //
    // El fallback de  es por asset y por ocurrencia: un pack con cien
    // assets rotos reintenta cien veces, cada frame. Eso funciona —se oye el
    // juego— pero paga la resolución completa por algo que ya se sabe que no va
    // a andar, y ése es el riesgo que  anota.
    //
    // Se cuentan ASSETS DISTINTOS, no ocurrencias. Un archivo roto que suena mil
    // veces es UN problema; doce archivos distintos es un pack mal armado o una
    // carpeta que no llegó. Contar ocurrencias apagaría el subsistema por un
    // solo asset que se repite mucho, que es justo el caso que NO hay que
    // castigar.
    void note_asset_failure(uint64_t key, const std::string& asset) {
        if (asset.empty()) return;
        // El subsistema sale del BUS de la firma (): un asset de música que
        // falta no dice nada sobre los efectos, y apagar los dos por uno sería
        // llevarse puesto lo que sí funciona.
        const Subsystem sub = (bus_of_signature(key) == AudioBus::Music)
                                  ? Subsystem::Music : Subsystem::Sfx;
        const uint32_t si    = static_cast<uint32_t>(sub);
        const size_t previous_count = escalation.count(si);
        const bool     cruzo = escalation.note(si, asset);
        // Sólo la PRIMERA vez que este asset falla: el mismo archivo se
        // reintenta cada frame, y loguearlo cada vez haría que el registro
        // creciera sin decir nada nuevo.
        if (escalation.count(si) == previous_count) return;

        // El PACK en el registro (): el asset y la causa ya estaban, pero
        // de qué pack venía, no — y con los nombres por hash () eso es lo
        // único que permite volver al proyecto que lo horneó.
        std::fprintf(stderr, "[degradación] %s no se pudo reproducir (pack: %s) "
                             "— %zu/%zu del subsistema\n",
                     asset.c_str(),
                     pack_path.empty() ? "(sin pack)" : pack_path.c_str(),
                     escalation.count(si), escalation.threshold());

        if (!cruzo) return;
        if (!sub_on(sub)) return;   // ya estaba apagado: nada que escalar
        subsystems_on   &= ~subsystem_bit(sub);
        auto_disabled_on |= subsystem_bit(sub);
        std::fprintf(stderr, "[degradación] %s APAGADO tras %zu archivos que no "
                             "se pudieron reproducir — se sigue con el original\n",
                     sub == Subsystem::Music ? "música HD" : "efectos HD",
                     escalation.count(si));
    }

    /// : ¿este sonido está silenciado por su BUS? Se pregunta por FIRMA,
    /// que es la identidad que tienen las dos mitades — el evento original y su
    /// reemplazo—, y por eso alcanza a las dos con una sola respuesta: la misma
    /// disciplina de .
    ///
    /// OJO con la asimetría respecto de , que es deliberada: silenciar un
    /// bus NO devuelve el original (eso es apagar el subsistema). Silenciar el
    /// bus de Música quiere decir «no quiero música», así que se va también la
    /// del juego.
    bool bus_muted_for(uint64_t sig) const {
        return bus_is_muted(bus_of_signature(sig));
    }
    /// Registrar el resultado de un disparo HD: un fallo deja de silenciar el
    /// original desde el frame siguiente (el router lo ve en ESTE mismo frame);
    /// un éxito re-arma el mute de la key. Devuelve `played` para encadenar.
    bool hd_fired(uint64_t key, bool played) {
        if (played) hd_failed_keys.erase(key);
        else        { hd_failed_keys.insert(key); ++hd_fallback; }
        return played;
    }

    /// : re-armar los reemplazos live tras una pausa (o al volver del
    /// bypass de Assets). NO toca audio_live_prev ni las ventanas: la
    /// instancia lógica sobrevivió al corte físico () y el stream se
    /// recrea desde el offset del reloj emulado — un evento sostenido
    /// vuelve a sonar sin esperar un key-on nuevo, un loop conserva su
    /// fase (módulo en el player) y una instancia vencida se descarta en
    /// vez de reiniciarse desde cero.
    void resume_live_instances() {
        if (!audio_enabled || !transport_playing || audio_live_bypass ||
            audio_live_inst.empty())
            return;
        const double fps = runner.fps();
        for (auto it = audio_live_inst.begin(); it != audio_live_inst.end(); ) {
            const uint64_t      key = it->first;
            const LiveInstance& li  = it->second;
            // Silenciado por el artista: la instancia sigue viva (el mute es
            // intención, no fin del evento) pero no suena.
            if (li.seq_sub) {
                const AudioSeqSub* sq = nullptr;
                for (const auto& s : audio_seq_subs)
                    if (s.key == key) { sq = &s; break; }
                if (!sq) { it = audio_live_inst.erase(it); continue; }  // sub quitada
                if (seq_sub_muted_live(*sq)) { ++it; continue; }
            } else if (signature_muted(key, li.ev_bit)) {
                ++it; continue;
            }
            // : asignado ≠ reproducible — con el asset roto/ausente suena
            // el original (su ventana ya no mutea); si el archivo aparece,
            // una próxima reanudación lo levanta.
            if (!hd_can_sound(key, li.asset)) { ++it; continue; }
            const bool in_pack =
                pack && ayther_pack_file_size(pack.get(), li.asset.c_str()) > 0;
            // La duración poda un one-shot ya drenado; para el pack la poda
            // la hace el player (offset pasado el final = éxito sin stream).
            const double dur = in_pack ? 0.0
                                       : audio.asset_duration_seconds(li.asset);
            const auto d = ayther::live_resume_decide(
                frame_index, li.start_frame, li.end_frame, li.cut_frame,
                li.looping, fps, dur);
            if (d.action == ayther::LiveResumeAction::Finished) {
                ++hd_resume_finished;
                it = audio_live_inst.erase(it);
                continue;
            }
            const bool played = in_pack
                // : la rama de disco ya respetaba `li.gain` y la del pack
                // no — el MISMO audio reanudado sonaba distinto segun de donde
                // saliera. El fade queda en 0 explicito: lo de esta rama es el
                // gain, no cambiar el contrato de fin.
                ? audio.play_event_hd(pack.get(), li.asset.c_str(), li.looping,
                                      key, li.end_frame, li.cut_frame,
                                      d.offset_seconds, 0u, li.gain)
                : audio.play_oneshot_asset_file(li.asset, key,
                                                d.offset_seconds, li.gain);
            if (hd_fired(key, played)) {
                ++hd_resumed;
                hd_resume_offset_frames += frame_index - li.start_frame;
                // : el one-shot de disco con tail finito vuelve a entrar
                // al barrido (la pausa pudo cruzar un toggle que lo limpió).
                if (!in_pack && !li.seq_sub && li.cut_frame != UINT64_MAX)
                    hd_oneshot_cut[key] = li.cut_frame;
            }
            ++it;
        }
    }

    /// : range-mute del catálogo del pack, transaccional — reemplaza al
    /// mute_at plano de AudioEventSubstitution. El frame se descarta solo si
    /// alguna sustitución que lo cubre está silenciada por el artista
    /// (intención) o tiene su asset del pack LISTO y sin fallo de arranque:
    /// un asset roto en el pack ya no deja su ventana en silencio.
    bool pack_evt_mute_at(uint64_t f) {
        const AytherAudioEventSub* s = audio_evt.subs();
        const uint32_t n = audio_evt.sub_count();
        for (uint32_t i = 0; i < n; ++i) {
            if (f < s[i].start_frame || f > s[i].end_frame) continue;
            if (signature_muted(s[i].signature, 0)) return true;
            if (!hd_failed_keys.count(s[i].signature) && pack &&
                audio.asset_ready_pack(pack.get(), s[i].asset_path))
                return true;
        }
        return false;
    }

    /// ¿Este evento YA tiene quien lo reemplace? Si lo tiene, su voz original
    /// no debe sonar — y ésa es toda la diferencia entre el modelo viejo y éste:
    /// antes había que ACERTAR una ventana de mute, ahora la voz simplemente no
    /// emite desde su propio key-on hasta el fin de su cola.
    ///
    /// : «lo tiene» exige el asset LISTO (hd_can_sound), no la mera
    /// asignación — con el asset roto la voz original vuelve a sonar desde su
    /// propio key-on (fallback transaccional). No-const: la consulta de
    /// readiness puede decodificar/re-statear la cache del player.
    bool voice_replaced(const AytherAudioEvent& e) {
        // Silenciado por el artista, por cualquiera de los tres ejes ().
        // Con el router puesto el chip calla ENTERO, así que las máscaras del
        // camino viejo (occurrence_mute_at, audio_manual_mute) ya no lo tocan:
        // si esta pregunta no las incluyera, el ojo de una Secuencia y el mute
        // por canal dejarían de silenciar nada.
        if (event_muted(e)) return true;
        // Sustituido por un asset HD, por evento o por Secuencia — esos suenan
        // por su propio camino (play_event_hd / play_oneshot_asset_file), así
        // que copiar el original acá sería oír los dos juntos. Eso era el
        // «suena degradado» del 2026-07-28. Solo si el asset PUEDE sonar
        // () — roto/ausente, la voz copia el original y se oye el juego.
        //  Fase 3: en bypass de Assets el reemplazo HD queda en suspenso
        // y la voz ORIGINAL suena — sin este gate, el router seguía callando
        // las voces asignadas con el toggle Assets apagado. La re-síntesis
        // SoundFont (abajo) no entra en el bypass: su synth sigue tocando.
        if (!audio_live_bypass) {
            //  F3: la asignación se resuelve exacta O por regla de
            // instrumento — la voz variante (misma voz en otra nota/canal)
            // también calla; su HD sale por el mismo camino que la exacta.
            uint64_t asig = 0;
            if (resolve_event_sig(e.signature, e.instrument, e.pitch, &asig))
                if (const auto it = audio_event_assign.find(asig);
                    it != audio_event_assign.end() &&
                    hd_can_sound(asig, it->second)) return true;
            for (const auto& sq : audio_seq_subs) {
                if (sq.asset.empty()) continue;
                if (std::find(sq.signatures.begin(), sq.signatures.end(),
                              e.signature) != sq.signatures.end() &&
                    hd_can_sound(sq.key, sq.asset)) return true;
            }
        }
        // Re-sintetizado con SoundFont (): su voz original calla. El DAC y
        // el ruido no aplican — ahí el SF2 no tiene nota que tocar.
        if (e.pitch != 255 && inst_assign.count(e.instrument)) return true;
        return false;
    }

    /// La pregunta de voice_replaced() para una voz EN VIVO (Capturar y el
    /// runtime del pack): no hay toma que alinear, así que la identidad viene
    /// del detector — la firma que le puso a este canal ESTE frame. Las
    /// preguntas son las mismas del replay: asignación resuelta exacta o por
    /// regla ( F3), ventanas per-firma con members () y ventanas de
    /// Secuencia de autoría, cada una con su gate transaccional () y el
    /// silencio del artista alcanzando al original ().
    /// : la FIRMA activa en este canal, o 0 si el detector no la ve. Es lo
    /// que deja saber de qué bus es la voz para escalarla — la misma pregunta
    /// que ya responde `bus_of_signature`, con la misma respuesta.
    uint64_t live_voice_signature(const VoiceContext& ctx) const {
        for (const auto& c : live_active)
            if (c.chip == ctx.chip && c.channel == ctx.channel) return c.signature;
        return 0;
    }

    bool live_voice_replaced(const VoiceContext& ctx) {
        const AytherAudioActive* a = nullptr;
        for (const auto& c : live_active)
            if (c.chip == ctx.chip && c.channel == ctx.channel) { a = &c; break; }
        // Voz que el detector no ve (residual, ajena): suena tal cual —
        // de más y no de menos, igual que el camino de replay.
        if (!a) return false;
        // El altavoz del Patrón silencia el timbre también en vivo — y antes
        // del bypass, como event_muted() en voice_replaced().
        if (a->instrument && audio_instrument_mute.count(a->instrument))
            return true;
        // : y el bus, por el mismo motivo y en el mismo lugar — un bus
        // silenciado calla la voz esté o no sustituida.
        if (bus_muted_for(a->signature)) return true;
        //  Fase 3: bypass de Assets = el juego original entero.
        if (audio_live_bypass) return false;
        const uint64_t sig = a->signature;
        // Asignación por evento (per-firma / packs), exacta o por regla.
        uint64_t asig = 0;
        if (resolve_event_sig(sig, a->instrument, a->pitch, &asig))
            if (const auto it = audio_event_assign.find(asig);
                it != audio_event_assign.end() &&
                hd_can_sound(asig, it->second)) return true;
        // Ventanas per-firma vivas (packs: [[event]] con duration/members).
        for (const auto& w : audio_seq_windows) {
            if (frame_index > w.end) continue;
            const auto wit = audio_event_assign.find(w.sig);
            if (wit == audio_event_assign.end() ||
                !hd_can_sound(w.sig, wit->second)) continue;   // 
            const auto mit = audio_event_members.find(w.sig);
            if (mit == audio_event_members.end() || mit->second.empty()) {
                // Packs viejos sin members: range-mute de la máscara.
                if (w.mask & chan_bit(ctx.chip, ctx.channel)) return true;
            } else if (sig == w.sig ||
                       std::find(mit->second.begin(), mit->second.end(), sig)
                           != mit->second.end()) {
                return true;
            }
        }
        // Ventanas de Secuencia de autoría (Lab): miembro exacto, disparadora,
        // o el timbre del disparador bajo regla ( F3 — la voz variante de
        // la transición calla junto con las exactas).
        for (const auto& w : audio_live_seq_win) {
            const AudioSeqSub* sq = nullptr;
            for (const auto& s : audio_seq_subs)
                if (s.key == w.key) { sq = &s; break; }
            if (!sq) continue;
            const bool member =
                sig == sq->trigger_signature ||
                std::find(sq->signatures.begin(), sq->signatures.end(), sig)
                    != sq->signatures.end() ||
                (sq->match_rule != AudioMatchRule::kExact &&
                 sq->match_instrument && a->instrument &&
                 a->instrument == sq->match_instrument);
            if (!member) continue;
            // El ojo silencia también al original (); sin HD sano el
            // original debe sonar ().
            if (seq_sub_muted_live(*sq) || hd_can_sound(w.key, sq->asset))
                return true;
        }
        // El SoundFont () no entra acá: su re-síntesis toca desde los
        // eventos analizados de la toma — en vivo no hay nota que reponer,
        // y callar el original sin reemplazo sería un mudo.
        return false;
    }

    /// La política del router. Ata lo que el router sabe —CUÁNDO arranca una voz
    /// y en qué canal— con lo que el artista asignó, que está indexado por firma
    /// y por instrumento. El puente son los eventos ya analizados.
    struct SessionPolicy final : VoicePolicy {
        Impl*        im = nullptr;
        SilentSource silent;
        /// : una fuente con ganancia POR BUS, no por voz. El factor se lee
        /// por render —apunta al array de la sesión— así que una sola alcanza
        /// para todas las voces de ese bus y no hay nada que sincronizar
        /// cuando el usuario mueve el slider.
        std::vector<GainSource> gains;

        /// Lo que suena cuando la voz NO está reemplazada: la copia de siempre,
        /// o la copia ESCALADA si su bus tiene el volumen bajo.
        ///
        /// nullptr sigue significando «copia tal cual», que es el contrato del
        /// router: el caso normal no paga ni una indirección.
        IVoiceSource* pass(uint64_t sig) {
            if (!sig) return nullptr;   // el detector no la ve: de más, no de menos
            const uint32_t b = static_cast<uint32_t>(im->bus_of_signature(sig));
            if (b >= kAudioBusCount) return nullptr;
            if (im->bus_gain[b] == 1.0f) return nullptr;
            return &gains[b];
        }

        IVoiceSource* choose(const VoiceContext& ctx) override {
            if (!im) return nullptr;
            // Mute por CANAL: incondicional, TODO el frame — ése es su
            // contrato, y por eso va ANTES del análisis. Con el chequeo sólo
            // dentro del loop de eventos, silenciar un canal para auditarlo
            // dejaba sonando todo key-on que el análisis no conociera.
            if (im->audio_manual_mute & chan_bit(ctx.chip, ctx.channel))
                return &silent;
            // EN VIVO (Capturar / runtime del pack): frame_index no alinea
            // con la toma analizada — el loop de abajo no encontraría el
            // evento (y una coincidencia numérica sería un match espurio de
            // otra escena). La verdad en vivo es el detector. Sin esta rama,
            // con el router puesto (default) el range-mute live moría en
            // `audio_mute = 0` y el original entero sonaba DEBAJO de los HD —
            // el «se cuelan originales además de los assets» de la transición
            // pantalla→demo (reporte 2026-08-10).
            if (im->audio_runtime_sub)
                return im->live_voice_replaced(ctx)
                           ? static_cast<IVoiceSource*>(&silent)
                           : pass(im->live_voice_signature(ctx));
            for (const AytherAudioEvent& e : im->audio_events) {
                if (e.start_frame != ctx.frame) continue;
                if (e.chip != ctx.chip || e.channel != ctx.channel) continue;
                return im->voice_replaced(e)
                           ? static_cast<IVoiceSource*>(&silent)
                           : pass(e.signature);
            }
            // Un key-on que el análisis no conoce (residual, o la toma sin
            // analizar) suena tal cual: de más y no de menos.
            return nullptr;
        }
    };
    SessionPolicy voice_policy;

    /// ¿El audio de este frame va a salir por el device? Es la MISMA compuerta
    /// que abre el flush; el router tiene que mirarla, no suponerla.
    bool voice_audible() const {
        return audio_enabled && !replay_quiet && audio_audible;
    }

    /// Guarda las escrituras de un frame para que el router las consuma.
    void voice_capture(const AytherAudioWrite* w, uint32_t n, uint32_t frame) {
        if (!voice_router_on) return;
        // : el playback secuencial extiende el caché de cebado gratis
        // (solo si es el frame contiguo a lo construido y de la toma dueña).
        if (replay_rec && voice_prime_rec == static_cast<const void*>(replay_rec))
            voice_prime_push(frame, w, n);
        // Un frame que NO va a sonar —el análisis de la toma re-emula 2000 y
        // pico, y los produce internos de la app agregan más— no aporta audio,
        // pero su ESTADO sí importa. Se ceba en el acto en vez de encolarse.
        //
        // Encolarlos era el bug del «suena degradado»: se acumulaban hasta el
        // tope, el primer flush rendía DOS SEGUNDOS de audio de una y, como el
        // device sólo consume un frame por vez, el resto se quedaba en el
        // resampler para siempre. Medido: atraso=94194 muestras = 2,14 s de
        // desfase con la imagen, y la cola en 129.
        if (!voice_audible()) {
            voice_router.mirror().prime_frame(w, n);
            voice_last_frame = (int64_t)frame_index;
            ++vr_skipped;
            return;
        }
        voice_pending.push_back({ frame, std::vector<AytherAudioWrite>(w, w + n) });
    }

    /// Corta TODO y vacía lo encolado. Para los seeks y los cortes de escena:
    /// sin esto quedan notas sonando sobre lo que sigue.
    void synth_panic() {
        for (auto& [_, s] : synths) ayther_sf2_all_notes_off(s);
        synth_on.clear();
        audio.clear_synth();
        synth_last_frame = -1;
    }

    /// Un frame de síntesis. Se llama SÓLO desde el flush de audio, o sea sólo
    /// cuando el PCM del emulador también sale — así el timbre no se adelanta
    /// ni queda sonando durante un produce interno.
    void synth_tick() {
        synth_pcm_ready = false;
        // Mismo gate que el muteo: con los Assets apagados no suena.
        if (!synth_any || !audio_sub_preview || audio_events.empty()) {
            if (!synth_on.empty()) synth_panic();
            return;
        }
        const int64_t f = (int64_t)frame_index;

        // Un salto ATRÁS, o uno más largo que el catch-up, es un seek o un
        // scrub: ahí hay que cortar o las notas del tramo viejo suenan sobre el
        // nuevo. Pero avanzar VARIOS frames de una no es un salto: es el
        // catch-up normal del playback. El Lab no corre a 16,7 ms por frame, así
        // que produce_frame saltea frames que igual se emulan en bare y cuyo PCM
        // VA AL DEVICE.
        //
        // Tratar eso como seek era catastrófico y por eso está medido: 490
        // panics en 707 frames de reproducción, o sea que casi toda nota se
        // apagaba a los pocos ms de arrancar. Ese era el «se escucha degradado,
        // o no se escucha, y pocas veces bien» (reporte 2026-07-27) — y no se
        // parecía en nada a lo que yo venía buscando adentro del sintetizador.
        constexpr int64_t kSynthCatchUpMax = 32;   // = kFastForwardMax del replay

        // Re-producir el MISMO frame no avanza nada, y este chequeo va PRIMERO.
        // produce_frame no es 1:1 con los frames emulados (compose,
        // replay_invalidate, export), así que repetir es lo NORMAL — y con el
        // chequeo después del de salto, cada repetición entraba por
        // `f <= synth_last_frame` y hacía panic, o sea que apagaba TODAS las
        // notas en vuelo. Ése era el «cuando asigno un SF2 a un sonido no se oye
        // bien» (reporte 2026-07-28): el timbre arrancaba y se cortaba enseguida.
        // Mismo bug que ya había aparecido en voice_tick, misma corrección.
        if (f == synth_last_frame) return;

        if (synth_last_frame >= 0 &&
            (f < synth_last_frame || f > synth_last_frame + kSynthCatchUpMax)) {
            ++syn_jumps;
            synth_panic();
        }
        const int64_t from = synth_last_frame < 0 ? f : synth_last_frame + 1;
        synth_last_frame = f;
        ++syn_ticks;

        // CUÁNTAS MUESTRAS. No 44100/fps: EXACTAMENTE las que el emulador
        // stageó, que con catch-up son las de VARIOS frames juntos.
        //
        // Un número fijo asume que el Lab corre a tiempo real, y no lo hace —
        // el probe de frame mide 20-33 ms donde el ideal es 16,7. Con fijo, el
        // stream del sintetizador recibe de menos y se muere de hambre (audio
        // entrecortado), mientras el del emulador se salva porque tiene DRC que
        // estira su ritmo. Atados al mismo número, los dos derivan igual y no
        // se separan nunca.
        //
        // El fallback sólo cubre el frame raro sin PCM del emulador.
        const double fps = runner.fps() > 1.0 ? runner.fps() : 60.0;
        size_t n = synth_frames_hint;
        if (n == 0) { ++syn_nopcm; n = (size_t)(44100.0 / fps + 0.5); }
        synth_pcm.assign(n * 2, 0.0f);

        // Los frames salteados por el catch-up SE PROCESAN igual —sus key-on y
        // key-off— y el render se reparte entre ellos. Aplicarlos todos juntos
        // al principio del bloque alcanzaría para que suenen, pero amontonaría
        // hasta medio segundo de notas en un instante; repartir mantiene el
        // orden y el tiempo aproximado.
        const int64_t span = f - from + 1;
        size_t done = 0;
        for (int64_t g = from; g <= f; ++g) {
            // Cierres primero: dos notas contiguas comparten frontera y abrir
            // antes de cerrar dejaría la vieja colgada.
            for (size_t i = synth_on.size(); i-- > 0;) {
                const SynthNote& sn = synth_on[i];
                // One-shot: no se cierra solo. Lo corta el re-disparo de abajo,
                // el corte global de un seek, o el mute de acá.
                //
                // El artista silenció este sonido (): su voz sintetizada
                // calla igual que la del chip. Incluye el one-shot EN VUELO —
                // si no se cierra acá no se cierra nunca, y seguía sonando
                // sobre el mute.
                //
                // Por OCURRENCIA y no sólo por timbre (): el ojo de una
                // Secuencia y el mute por canal también son «silenciar», y con
                // el chequeo por instrumento nada más el SoundFont seguía
                // tocando la ocurrencia que el artista acababa de apagar.
                const bool muted = occurrence_muted(sn.occ, sn.inst);
                if (sn.one_shot && !muted) continue;
                // Cierra cuando termina ESTA NOTA, no cuando el instrumento
                // deja de sonar. Preguntar por el instrumento era el bug del
                // «después se reproduce casi continuo sostenido sin variar el
                // tono» (reporte 2026-07-28): entre dos notas seguidas del mismo
                // timbre SIEMPRE hay un evento cubriendo el frame, así que la
                // primera nunca se cerraba y la siguiente sumaba una voz más.
                // Se apilaban hasta volverse un acorde sostenido donde ya no se
                // distingue la melodía. El chip hace exactamente esto: cada
                // evento es un key-on y su key-off.
                if (!muted && g <= sn.ends) continue;
                auto it = inst_assign.find(sn.inst);
                if (it != inst_assign.end()) {
                    if (AytherSf2* sy = synth_for(sn.inst))
                        { ayther_sf2_note_off(sy, sn.ch, sn.key); ++syn_off; }
                }
                synth_on.erase(synth_on.begin() + (long)i);
            }

            for (const AytherAudioEvent& e : audio_events) {
                if ((int64_t)e.start_frame != g) continue;
                if (e.pitch == 255) continue;          // DAC/ruido: no hay nota
                auto it = inst_assign.find(e.instrument);
                if (it == inst_assign.end()) continue;
                // Silenciar un sonido lo silencia ENTERO (, ). Antes el
                // mute sólo llegaba al chip y el SoundFont seguía tocando: el
                // panel decía «silenciado» y se oía igual. Los tres ejes, no
                // sólo el instrumento — ver event_muted.
                //
                // El chequeo va DESPUÉS del inst_assign para que `syn_muted`
                // cuente lo que dice contar: notas sintetizadas que no sonaron
                // por el mute. Antes iba primero y sumaba también los timbres
                // sin SoundFont, que no habrían sonado igual — el contador
                // marcaba 301 con una sola asignación viva.
                if (event_muted(e)) { ++syn_muted; continue; }
                // Prioridad ASSET > SF2: si un asset (por firma o por
                // Secuencia activa) ya toca este evento, el SoundFont calla —
                // superpuestos era el reporte 2026-07-31.
                if (event_covered_by_asset(e, (uint32_t)g)) continue;
                AytherSf2* sy = synth_for(e.instrument);
                if (!sy) continue;
                // Canal 0 SIEMPRE: cada timbre tiene su propio sintetizador, así
                // que ya no hay que repartirlos entre canales MIDI.
                const int ch = 0;
                int key = (int)e.pitch + it->second.transpose;
                if (key < 0 || key > 127) continue;    // fuera de rango: se descarta
                const int vel = e.velocity ? (int)e.velocity : 100;
                // Re-disparo de la MISMA nota que sigue en vuelo (un golpe que
                // se repite rápido): se cierra primero, o el sintetizador
                // acumula voces del mismo tono que se suman y saturan.
                for (size_t i = synth_on.size(); i-- > 0;) {
                    if (synth_on[i].ch != ch || synth_on[i].key != key) continue;
                    ayther_sf2_note_off(sy, ch, key);
                    ++syn_off;
                    synth_on.erase(synth_on.begin() + (long)i);
                }
                // Ganancia del timbre (): CC 7 del canal, ANTES del
                // note_on, y no una sola vez al asignar: el artista la mueve
                // con el slider y el cambio tiene que oírse sin re-asignar.
                //
                // 1.0 → 127, o sea NEUTRO: el timbre entra al nivel que tiene el
                // SoundFont, y desde ahí se baja. Antes 1.0 mandaba 100, que ya
                // es −4 dB de arranque; con el A/B no se notaba porque el timbre
                // sonaba solo, pero conviviendo con la mezcla del router quedaba
                // 18 dB por debajo del juego (medido con AYTHER_SF2_DUMP:
                // −45 dBFS contra −27). Un default no debería atenuar.
                // La ganancia se parte en dos porque MIDI no puede realzar:
                // hasta 1.0 baja por CC 7 (0-127), y por encima sube escalando
                // el render de ESTE sintetizador — exacto, porque atiende a una
                // sola voz (). 1.0 es el centro: ni atenúa ni realza.
                const float gn = it->second.gain;
                ayther_sf2_control(sy, ch, 7,
                    std::clamp(static_cast<int>(std::min<float>(gn, 1.0f) * 127.0f + 0.5f),
                               0, 127));
                const bool one_shot = synth_one_shot(e.instrument);
                ayther_sf2_note_on(sy, ch, key, vel);
                ++syn_on;
                synth_on.push_back({ e.instrument,
                                     occ_key(e.chip, e.channel, e.start_frame),
                                     ch, key, g, (int64_t)e.end_frame, one_shot });
            }

            const size_t upto = std::min<size_t>(
                n, (size_t)((double)n * (double)(g - from + 1) / (double)span));
            if (upto <= done) continue;
            const size_t chunk = upto - done;
            for (auto& [inst, sy] : synths) {
                // Un sintetizador por TIMBRE: se renderizan todos y se suman,
                // cada uno con SU realce. Escalar acá es exacto justamente
                // porque el sintetizador atiende a una sola voz ().
                static std::vector<float> tmp;
                tmp.assign(chunk * 2, 0.0f);
                ayther_sf2_render(sy, tmp.data(), chunk);
                const auto bit = synth_boost.find(inst);
                const float g = bit == synth_boost.end() ? 1.0f : bit->second;
                for (size_t i = 0; i < tmp.size(); ++i)
                    synth_pcm[done * 2 + i] += tmp[i] * g;
            }
            done = upto;
        }
        // Con el router puesto NO se encola acá: los dos alimentan el mismo
        // stream y encolar de a dos le daría el doble de muestras por frame. Se
        // deja listo y voice_tick lo SUMA a su mezcla — los dos producen
        // exactamente `n` muestras, así que sumar es trivial y queda un solo
        // camino de salida.
        if (voice_router_on) { synth_pcm_ready = true; return; }
        audio.feed_synth(synth_pcm.data(), n);
    }

    /// Un frame del router de voces (). Se llama desde el MISMO lugar que
    /// synth_tick —el flush de audio— por el mismo motivo: sólo cuando el PCM
    /// del emulador también sale, o el audio se adelanta durante un produce
    /// interno.
    ///
    /// Devuelve si dejó SU bloque en el staging del player. Importa: el router
    /// no silencia al chip, le OCUPA EL LUGAR (buffer_router pisa el PCM
    /// staged), así que un frame en el que el router no rinde es un frame en el
    /// que lo staged sigue siendo del chip — y hay que tirarlo, no empujarlo.
    bool voice_tick() {
        if (!voice_router_on) return false;
        const int64_t f = (int64_t)frame_index;

        // Un salto ATRÁS, o uno más largo que el catch-up, es un seek. Esta es
        // la lección cara de synth_tick: avanzar VARIOS frames de una NO es un
        // salto, es el catch-up normal — el Lab no corre a 16,7 ms por frame.
        // Tratarlo como seek daba 490 panics en 707 frames de reproducción.
        constexpr int64_t kVoiceCatchUpMax = 32;   // = kFastForwardMax del replay

        // Re-producir el MISMO frame no avanza nada, y este chequeo va PRIMERO.
        // Con él después del de salto, una pausa cebaba la toma entera en CADA
        // frame —f <= voice_last_frame es cierto al repetir— y el frame se iba a
        // 100 ms. produce_frame no es 1:1 con los frames emulados (compose,
        // replay_invalidate, export), así que repetir es lo normal, no la
        // excepción.
        if (f == voice_last_frame) { voice_pending.clear(); return false; }

        // : llegar SIN historia (voice_last_frame < 0 — sesion nueva o
        // despues de replay_invalidate) a un frame que no es el 0 es el salto
        // MAS frio que existe, y sin embargo no contaba como salto: la
        // condicion exigia `>= 0`, asi que el espejo se quedaba sin cebar y
        // sintetizaba con los registros en reset. Eso es el audio DELGADO del
        // reporte — medido con tools/audio_seek_probe: -74,3 % de nivel.
        //
        // Es la misma clase de defecto que el espejo mudo al arranque de la
        // toma: el caso «no hay estado previo» se leia como «no hace falta
        // reponer nada», cuando es exactamente al reves.
        const bool jumped = (voice_last_frame < 0 && f > 0) ||
                            (voice_last_frame >= 0 &&
                             (f < voice_last_frame ||
                              f > voice_last_frame + kVoiceCatchUpMax));
        if (jumped) {
            // A diferencia del SoundFont, acá no alcanza con callar las notas:
            // el espejo necesita el ESTADO DE REGISTROS del punto al que se
            // saltó, o el timbre sale mal (Fase 0: la correlación de envolvente
            // cae de 0,975 a 0,889 arrancando en frío). Se reconstruye
            // recorriendo las escrituras de la toma sin generar audio.
            //
            // Lo pendiente de ESTE frame SOBREVIVE al cebado (). El cebado
            // reconstruye [0, f) desde la toma y después limpia la cola — pero
            // el produce que está corriendo ya capturó las escrituras del frame
            // f, que son las únicas que el cebado NO cubre. Tirarlas dejaba al
            // router sin el key-on del frame al que se saltó: la voz no
            // arrancaba, la política nunca se consultaba (por eso no callaba
            // nada) y el canal quedaba mudo hasta el próximo key-on. Se notaba
            // justo donde más importa —saltar al inicio de una Secuencia, que
            // es el frame del key-on— y en el replay de una toma recién
            // analizada, donde el análisis deja el cursor al final y el primer
            // frame reproducido es siempre un salto.
            std::vector<PendingFrame> keep;
            for (auto& pf : voice_pending)
                if ((int64_t)pf.frame >= f) keep.push_back(std::move(pf));
            voice_prime_to((uint32_t)f);
            voice_pending = std::move(keep);
        }
        voice_last_frame = f;
        ++vr_ticks;

        // Rendir TODOS los frames pendientes —los bare del catch-up incluidos—
        // a la tasa del chip, y recién después convertir de una.
        // CUÁNTAS MUESTRAS: exactamente las que stageó el emulador, igual que el
        // sintetizador. Atados al mismo número, los dos streams derivan igual y
        // no se separan nunca; con un número fijo, el del router se muere de
        // hambre porque no tiene el DRC que salva al del emulador.
        const double fps = runner.fps() > 1.0 ? runner.fps() : 60.0;
        size_t n = synth_frames_hint;
        if (n == 0) n = (size_t)(44100.0 / fps + 0.5);

        // Rendir MÁS frames de los que el device va a consumir es atraso que no
        // se recupera nunca (el resampler lo guarda y en régimen entra tanto
        // como sale). Lo que sobra se ceba: su estado entra, su audio no.
        // Un frame de holgura, no cero: apretado al límite exacto el resampler
        // se quedaba corto en el 1% de los bloques (necesita media ventana de
        // lookahead) y esos bloques salían con la cola en silencio — micro-cortes.
        // Un frame de más son ~17 ms de latencia, imperceptibles.
        const size_t budget = (size_t)((double)n * fps / 44100.0 + 2.5);
        while (voice_pending.size() > budget) {
            voice_router.mirror().prime_frame(voice_pending.front().writes.data(),
                                              (uint32_t)voice_pending.front().writes.size());
            voice_pending.erase(voice_pending.begin());
            ++vr_skipped;
        }

        voice_chip.clear();
        vr_pend_max = std::max<size_t>(vr_pend_max, voice_pending.size());
        std::vector<float> blk;
        for (auto& pf : voice_pending) {
            voice_router.tick(pf.writes.data(), (uint32_t)pf.writes.size(), pf.frame, blk);
            voice_chip.insert(voice_chip.end(), blk.begin(), blk.end());
            ++vr_frames;
        }
        voice_pending.clear();
        voice_rs.push(voice_chip.data(), voice_chip.size() / 2);

        // Red de seguridad: si aun así se juntó atraso, tirarlo. Un corte se oye
        // una vez; dos segundos de desfase con la imagen se oyen siempre. No
        // debería dispararse con lo de arriba puesto — si vr_resyncs sube, hay
        // otra fuente de desbalance y se ve acá en vez de deducirse de oído.
        if (voice_rs.available() > n * 3) { voice_rs.reset(); ++vr_resyncs; }
        voice_out.assign(n * 2, 0.0f);
        const size_t got = voice_rs.pull(voice_out.data(), n);
        // Faltó entrada: se completa con silencio y se cuenta. Si esto sube, el
        // router está entregando de menos y hay que mirar el pacing, no el
        // sonido — es el detector que a  le faltó durante diez causas raíz.
        if (got < n) ++vr_starved;
        // La voz del SoundFont entra acá (synth_tick corrió recién y dejó su
        // bloque listo): un solo camino de salida, sin dos productores peleando
        // por el stream.
        if (synth_pcm_ready && synth_pcm.size() >= n * 2) {
            double acc = 0.0;
            for (size_t i = 0; i < n * 2; ++i) {
                voice_out[i] += synth_pcm[i];
                acc += double(synth_pcm[i]) * synth_pcm[i];
            }
            // Nivel del bloque del SoundFont, para poder distinguir «no suena»
            // de «suena bajo» de «suena y se corta» sin depender del oído.
            const double r = n ? std::sqrt(acc / double(n * 2)) : 0.0;
            if (r > vr_sf2_peak) vr_sf2_peak = r;
            vr_sf2_rms = r;
            if (sf2_dump) {
                std::fwrite(synth_pcm.data(), sizeof(float), n * 2, sf2_dump);
                std::fflush(sf2_dump);
            }
        } else if (sf2_dump) {
            // Sin bloque del SF2 va SILENCIO, para que el archivo conserve el
            // eje de tiempo: si no, los huecos se cerrarían y no se vería que
            // el timbre dejó de sonar.
            static std::vector<float> z;
            z.assign(n * 2, 0.0f);
            std::fwrite(z.data(), sizeof(float), n * 2, sf2_dump);
        }
        vr_backlog = voice_rs.available();
        vr_queued  = audio.synth_queued_frames();
        if (voice_dump) {
            std::fwrite(voice_out.data(), sizeof(float), n * 2, voice_dump);
            std::fflush(voice_dump);
        }
        // El router OCUPA EL LUGAR del PCM del emulador: `buffer_router` pisa lo
        // staged. Así hereda todo el pacing de  —DRC y re-cebado tras un
        // stall— en vez de correr en un stream propio que no tiene nada: medido,
        // con stream propio la cola quedaba en 0 en casi todos los ticks, o sea
        // un corte por cada tirón del Lab.
        //
        // Y ESTE reemplazo es TODO lo que calla al chip (). Antes se lo
        // muteaba además a nivel core (0x3FF), y eso tenía un daño invisible:
        // el hasher de audio se alimenta del PCM del core, así que veía silencio
        // y las tomas grabadas con el router puesto salían SIN hashes de audio —
        // sin sustitución por lote y sin historia de audio. Se cazó al prenderlo
        // por defecto (audio_preview_smoke: «0 hashes de audio»).
        //
        // : en Sega CD no ocupa el lugar, se SUMA — ahí lo staged lleva el
        // chip PCM y el CDDA, que este router no espeja, y reemplazarlo dejaba
        // el sistema mudo. Lo que el router sí rinde ya viene callado del core.
        audio.buffer_router(voice_out.data(), n, router_mix());

        // Cada ~5 s. No por frame: un fprintf por frame ya se llevó el 92% del
        // «costo de upload» una vez (), y acá el dato no cambia tan rápido.
        if (vr_ticks % 300 == 0)
            std::fprintf(stdout,
                "[voice] ticks=%llu frames_chip=%llu cebados=%llu sin_entrada=%llu "
                "sustituidas=%llu atraso=%zu cola_max=%zu cebados_sueltos=%llu "
                "resyncs=%llu sf2_rms=%.5f sf2_pico=%.5f\n",
                (unsigned long long)vr_ticks, (unsigned long long)vr_frames,
                (unsigned long long)vr_primes, (unsigned long long)vr_starved,
                (unsigned long long)voice_router.stats().substituted,
                vr_backlog, vr_pend_max,
                (unsigned long long)vr_skipped, (unsigned long long)vr_resyncs,
                vr_sf2_rms, vr_sf2_peak);
        return true;
    }

    /// Escrituras de chip de TODA la toma, cacheadas por analyze_audio_events
    /// (que ya hace una pasada completa re-emulando). Es lo único con lo que se
    /// puede reconstruir el estado del espejo tras un seek: la grabación guarda
    /// inputs, no escrituras, así que sin este caché habría que re-emular.
    ///
    /// El tope existe porque un juego cargado de DAC escribe ~660 por frame:
    /// una toma larga llegaría a cientos de MB. Pasado el tope se descarta todo
    /// y el cebado degrada a reset — se oye un timbre equivocado un momento tras
    /// un seek, que es mucho mejor que quedarse sin memoria.
    static constexpr size_t kVoicePrimeCap = 4u * 1000u * 1000u;   // ~32 MB
    std::vector<AytherAudioWrite> voice_prime_writes;
    std::vector<uint32_t>         voice_prime_offsets;   // frame → índice de inicio
    bool                          voice_prime_capped = false;
    /// DUEÑO del caché: la toma cuyas escrituras están cacheadas. Sin esto,
    /// dos bugs: (a) una toma con audio_events.toml persistido nunca
    /// re-analiza → caché vacío → el espejo arranca EN FRÍO tras un seek (sin
    /// los patches FM del inicio de la canción, que no se reescriben nunca,
    /// el título quedaba fino/agudo para SIEMPRE — reporte 2026-08-19); y
    /// (b) cambiar de toma dejaba el caché de la anterior y el cebado primaba
    /// con las escrituras EQUIVOCADAS. replay_seek lo construye perezoso.
    const void*                   voice_prime_rec = nullptr;

    /// /: el caché se construye INCREMENTAL y acotado a `upto`, no
    /// de una pasada por toda la toma — con «Ax Game Play» (34.892 frames)
    /// la pasada completa eran 32 s de cuelgue sincrónico en el primer seek
    /// (el «playhead clavado» de ). Invariante: `voice_prime_offsets`
    /// tiene built+1 entradas = las escrituras de los frames [0, built).
    /// Quien extiende: (a) el playback secuencial, frame a frame y gratis
    /// (voice_prime_append desde voice_capture), (b) un seek más allá de lo
    /// construido, re-emulando bare SOLO el tramo [start, upto) con el mejor
    /// arranque que le pasa replay_seek (keyframe runtime/horneado o el
    /// savestate propio del final de lo construido).
    std::vector<uint8_t> voice_prime_state;        ///< máquina al final de lo construido
    uint32_t             voice_prime_state_frame = UINT32_MAX;   ///< frame de ese estado (built en su momento)

    /// Extiende el caché hasta `upto` sin ayuda del caller (para los
    /// exports): arranque = savestate propio si coincide con built, si no
    /// el estado inicial de la toma (re-corre bare lo ya cacheado).
    void voice_prime_ensure(uint32_t upto) {
        if (!replay_rec || voice_prime_rec != static_cast<const void*>(replay_rec)) return;
        const AytherRecording& rec = *replay_rec;
        const uint32_t built = voice_prime_built();
        if (voice_prime_capped || built >= (std::min)(upto, rec.frame_count())) return;
        if (!voice_prime_state.empty() && voice_prime_state_frame == built) {
            const std::vector<uint8_t> st = voice_prime_state;
            voice_prime_build(rec, upto, built, &st);
        } else {
            voice_prime_build(rec, upto, 0, &rec.initial_state);
        }
        replay_pos = -1;   // la máquina quedó en otro frame: el próximo seek re-posiciona
    }
    uint32_t voice_prime_built() const {
        return voice_prime_offsets.empty() ? 0u
             : static_cast<uint32_t>(voice_prime_offsets.size() - 1);
    }
    void voice_prime_reset(const AytherRecording& rec) {
        voice_prime_rec = &rec;
        voice_prime_writes.clear();
        voice_prime_offsets.assign(1, 0u);
        voice_prime_capped = false;
        voice_prime_state.clear();
        voice_prime_state_frame = UINT32_MAX;
    }
    /// Agrega las escrituras del frame `frame` si es EXACTAMENTE el siguiente
    /// a lo construido (contiguo). Devuelve true si las tomó.
    bool voice_prime_push(uint32_t frame, const AytherAudioWrite* w, uint32_t wc) {
        if (voice_prime_capped || voice_prime_offsets.empty()) return false;
        if (frame != voice_prime_built()) return false;
        if (voice_prime_rec && frame >= static_cast<const AytherRecording*>(voice_prime_rec)->frame_count())
            return false;
        if (wc) {
            if (voice_prime_writes.size() + wc > kVoicePrimeCap) {
                voice_prime_capped = true;
                voice_prime_writes.clear();
                voice_prime_writes.shrink_to_fit();
                voice_prime_offsets.clear();
                voice_prime_state.clear();
                std::fprintf(stdout,
                    "[voice] toma demasiado larga para cachear escrituras: "
                    "el cebado tras un seek degrada a reset\n");
                return false;
            }
            voice_prime_writes.insert(voice_prime_writes.end(), w, w + wc);
        }
        voice_prime_offsets.push_back(static_cast<uint32_t>(voice_prime_writes.size()));
        return true;
    }
    /// Extiende el caché hasta `upto` (exclusivo) re-emulando bare desde
    /// `start` con `start_state` (start ≤ built: los frames [start, built)
    /// se corren sin capturar). Deja la máquina al final del tramo: el
    /// caller la reposiciona (replay_seek lo hace siempre). Guarda un
    /// savestate al final para que la próxima extensión no vuelva a 0.
    void voice_prime_build(const AytherRecording& rec, uint32_t upto,
                           uint32_t start, const std::vector<uint8_t>* start_state) {
        if (voice_prime_rec != &rec) voice_prime_reset(rec);
        const uint32_t n = rec.frame_count();
        if (upto > n) upto = n;
        if (voice_prime_capped) return;
        const uint32_t built = voice_prime_built();
        if (built >= upto) return;
        if (!start_state || start_state->empty() || start > built ||
            !runner.unserialize(*start_state)) {
            voice_prime_offsets.clear();   // sin arranque válido: cache inválido → fallback a reset
            return;
        }
        for (uint32_t f = start; f < upto; ++f) {
            runner.set_input(0, rec.inputs[f]);
            runner.run_frame();
            if (f < built) continue;          // ya cacheado: solo avanzar la máquina
            const AytherAudioWrite* w = nullptr;
            uint32_t wc = 0;
            ayther_frame_snapshot_v1 bs{};
            if (runner.capture_frame_snapshot(bs).ok()) {
                abi_audio.resize(bs.audio_write_count);
                const auto rb = runner.read_audio_writes_v1(
                    abi_audio.data(), static_cast<uint32_t>(abi_audio.size()), bs);
                if (rb.ok()) {
                    w  = reinterpret_cast<const AytherAudioWrite*>(abi_audio.data());
                    wc = rb.count;
                }
            } else {
                AYTHER_LEGACY_READ_BEGIN
                w  = reinterpret_cast<const AytherAudioWrite*>(runner.audio_writes());
                wc = runner.audio_write_count();
                AYTHER_LEGACY_READ_END
            }
            if (!voice_prime_push(f, w, wc)) return;   // capped
        }
        if (upto < n) {
            std::vector<uint8_t> st;
            if (runner.serialize(st) && !st.empty()) {
                voice_prime_state       = std::move(st);
                voice_prime_state_frame = upto;
            }
        } else {
            voice_prime_state.clear();
            voice_prime_state_frame = UINT32_MAX;
        }
    }

    /// Reconstruye el estado de registros del espejo hasta `target` SIN generar
    /// audio. Barato: son escrituras a registros, no síntesis.
    /// : cuantos frames del cebado se RINDEN de verdad (el resto solo
    /// repone registros). Dos segundos a 60 fps — ver voice_prime_to().
    static constexpr uint32_t kVoicePrimeRenderFrames = 120;

    void voice_prime_to(uint32_t target) {
        voice_router.reset();
        voice_rs.reset();
        voice_pending.clear();
        ++vr_primes;
        if (voice_prime_offsets.size() < 2) return;
        const uint32_t last = std::min<uint32_t>(
            target, uint32_t(voice_prime_offsets.size() - 1));
        // : los ULTIMOS frames del cebado se RINDEN (audio generado y
        // descartado) en vez de solo reponer registros.
        //
        // POR QUE. `prime_frame` escribe los registros al chip y adelanta el
        // reloj SIN generar muestras (`generated_ = frame_base_/...`), asi que
        // las ENVOLVENTES nunca evolucionan: una nota que llevaba tres segundos
        // sonando llegaba al frame destino con su envolvente en el arranque, y
        // el espejo entregaba una version DELGADA de la musica. Medido con
        // `tools/audio_seek_probe` sobre «Musica intro» f0=400: -74,3 % de
        // nivel con el router puesto, y 0,0 % con el router apagado — el chip
        // del emulador nunca fue el problema.
        //
        // Rendir TODO el tramo seria correcto y carisimo (un seek al minuto 5
        // son 18.000 frames de sintesis). Con dos segundos alcanza: es varias
        // veces el attack+decay mas largo del YM2612, asi que la envolvente
        // llega al mismo sustain que llegaria emulando desde el principio.
        const uint32_t warm = last < kVoicePrimeRenderFrames
                                  ? last : kVoicePrimeRenderFrames;
        const uint32_t cut  = last - warm;
        for (uint32_t i = 0; i < last; ++i) {
            const uint32_t a = voice_prime_offsets[i];
            const uint32_t b = voice_prime_offsets[i + 1];
            if (b <= a) continue;
            if (i < cut)
                voice_router.mirror().prime_frame(voice_prime_writes.data() + a, b - a);
            else
                voice_router.mirror().run_frame(voice_prime_writes.data() + a, b - a);
        }
    }

    AytherSf2* synth_for(uint64_t inst) {
        auto it = synths.find(inst);
        return it == synths.end() ? nullptr : it->second;
    }

    /// Carga un SoundFont (pack primero, disco después — mismo criterio que
    /// sprites y poses: el artista asigna archivos sueltos y ESCUCHA antes de
    /// hornear) y devuelve una instancia NUEVA de sintetizador. El parse se
    /// comparte por clave (ayther_sf2_new_shared), así que N instancias del
    /// mismo archivo no lo duplican. nullptr si no se pudo — el caller decide
    /// si loguea.
    AytherSf2* load_sf2_shared(const std::string& sf) {
        if (sf.empty()) return nullptr;
        const uint64_t sfkey = std::hash<std::string>{}(sf);
        if (pack) {
            const int64_t sz = ayther_pack_file_size(pack.get(), sf.c_str());
            if (sz > 0) {
                std::vector<uint8_t> buf(static_cast<size_t>(sz));
                if (ayther_pack_read(pack.get(), sf.c_str(), buf.data(), buf.size()) > 0)
                    if (AytherSf2* sy = ayther_sf2_new_shared(sfkey, buf.data(),
                                                              buf.size(), 44100))
                        return sy;
            }
        }
        // Un .sfz se normaliza por RUTA (sus samples viven al lado del texto);
        // .sf2/.sf3 van crudos — la conversión SF3 la hace ayther_sf2_new_shared
        // por detección de bytes, una sola vez por key (queda en su cache).
        const bool sfz = sf.size() > 4 &&
            [&] {
                std::string ext = sf.substr(sf.size() - 4);
                for (auto& c : ext)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                return ext == ".sfz";
            }();
        if (sfz) {
            const size_t need =
                ayther_soundfont_normalize_file(sf.c_str(), nullptr, 0);
            if (need == 0) return nullptr;
            std::vector<uint8_t> norm(need);
            if (ayther_soundfont_normalize_file(sf.c_str(), norm.data(),
                                                norm.size()) != need)
                return nullptr;
            return ayther_sf2_new_shared(sfkey, norm.data(), norm.size(), 44100);
        }
        std::FILE* fh = std::fopen(sf.c_str(), "rb");
        if (!fh) return nullptr;
        std::fseek(fh, 0, SEEK_END);
        const long fsz = std::ftell(fh);
        std::fseek(fh, 0, SEEK_SET);
        AytherSf2* sy = nullptr;
        if (fsz > 0) {
            std::vector<uint8_t> fbuf(static_cast<size_t>(fsz));
            if (std::fread(fbuf.data(), 1, fbuf.size(), fh) == fbuf.size())
                sy = ayther_sf2_new_shared(sfkey, fbuf.data(), fbuf.size(), 44100);
        }
        std::fclose(fh);
        return sy;
    }

    /// : la base del export por el ROUTER, offline. Las MISMAS decisiones
    /// que la preview (SessionPolicy para las voces del chip + las reglas de
    /// synth_tick para el SoundFont) aplicadas sobre el cache de escrituras de
    /// la toma — sin device, sin pacing, sin DRC. La preview y el entregable
    /// dejan de ser dos renders distintos, y el residuo que el detector no
    /// puede modelar (: ring post key-off, pokes sin key) no llega al WAV:
    /// una voz silenciada no EMITE, no hay ventana que acertar.
    ///
    /// Instancias PROPIAS de router y sintetizadores: el vivo puede estar
    /// sonando mientras se exporta y no se le puede tocar ni el estado del
    /// espejo ni las voces en vuelo.
    ///
    /// Devuelve false si no puede (router apagado, toma sin analizar, cache de
    /// escrituras capped) — el caller cae al camino viejo (máscara sustractiva
    /// sobre el emulador).
    bool export_router_base(uint32_t start, uint32_t win, double fps,
                            size_t max_frames, std::vector<int16_t>& out) {
        out.clear();
        if (!voice_router_on || audio_events.empty()) return false;
        // : el caché es incremental — un export más allá de lo
        // construido lo extiende (offline: desde el savestate propio si está
        // al final de lo construido, si no desde el inicio de la toma).
        voice_prime_ensure(start + win);
        if (voice_prime_capped || voice_prime_offsets.size() < 2) return false;
        const uint32_t cached = static_cast<uint32_t>(voice_prime_offsets.size() - 1);
        if (start >= cached) return false;
        const uint32_t last = (std::min)(start + win, cached);

        ChannelRouter router;
        SessionPolicy pol;
        pol.im = this;
        router.set_policy(&pol);
        const double sfps = runner.fps();
        router.mirror().set_pal(sfps > 1.0 && sfps < 55.0);
        for (uint32_t i = 0; i < start; ++i) {
            const uint32_t a = voice_prime_offsets[i], b = voice_prime_offsets[i + 1];
            if (b > a) router.mirror().prime_frame(voice_prime_writes.data() + a, b - a);
        }
        StreamResampler rs;
        rs.set_rates(router.mirror().rate(), 44100.0);

        // SoundFont offline: sintetizadores FRESCOS por timbre y notas en
        // vuelo con las reglas de synth_tick — sin su catch-up, porque acá
        // cada frame es un frame. Mismo gate que el vivo (synth_tick:861).
        const bool with_synth = synth_any && audio_sub_preview;
        std::unordered_map<uint64_t, AytherSf2*> osy;
        auto osy_for = [&](uint64_t inst) -> AytherSf2* {
            auto it = osy.find(inst);
            if (it != osy.end()) return it->second;
            AytherSf2* sy = nullptr;
            const auto ia = inst_assign.find(inst);
            if (ia != inst_assign.end()) {
                sy = load_sf2_shared(ia->second.soundfont);
                if (sy) ayther_sf2_program(sy, 0, ia->second.preset);
            }
            osy.emplace(inst, sy);   // se cachea aunque sea nulo (no reintentar)
            return sy;
        };
        struct ONote { uint64_t inst, occ; int key; int64_t ends; bool one_shot; };
        std::vector<ONote> on;

        std::vector<float> blk, mixf, tmp;
        // Saca del resampler lo disponible, suma el bloque del SoundFont
        // (mismo largo — un solo camino de salida, como voice_tick) y convierte
        // EXACTAMENTE como buffer_router: lrint ×32767 con clamp.
        auto flush_avail = [&]() {
            const size_t avail = rs.available();
            if (!avail || out.size() / 2 >= max_frames) return;
            mixf.assign(avail * 2, 0.0f);
            rs.pull(mixf.data(), avail);
            if (with_synth) {
                for (auto& [inst, sy] : osy) {
                    if (!sy) continue;
                    tmp.assign(avail * 2, 0.0f);
                    ayther_sf2_render(sy, tmp.data(), avail);
                    const auto ia = inst_assign.find(inst);
                    const float g = (ia != inst_assign.end() && ia->second.gain > 1.0f)
                                        ? ia->second.gain : 1.0f;
                    for (size_t i = 0; i < tmp.size(); ++i) mixf[i] += tmp[i] * g;
                }
            }
            const size_t room = max_frames * 2 - out.size();
            const size_t take = (std::min)(mixf.size(), room);
            for (size_t i = 0; i < take; ++i) {
                const int v = static_cast<int>(std::lrint(mixf[i] * 32767.0f));
                out.push_back(static_cast<int16_t>(
                    v > 32767 ? 32767 : (v < -32768 ? -32768 : v)));
            }
        };

        for (uint32_t f = start; f < last && out.size() / 2 < max_frames; ++f) {
            const uint32_t a = voice_prime_offsets[f], b = voice_prime_offsets[f + 1];
            router.tick(voice_prime_writes.data() + a, b - a, f, blk);
            rs.push(blk.data(), blk.size() / 2);

            if (with_synth) {
                // Cierres primero (dos notas contiguas comparten frontera), y
                // por nota, no por instrumento — ver la saga en synth_tick.
                for (size_t i = on.size(); i-- > 0;) {
                    const ONote& sn = on[i];
                    const bool muted = occurrence_muted(sn.occ, sn.inst);
                    if (sn.one_shot && !muted) continue;
                    if (!muted && static_cast<int64_t>(f) <= sn.ends) continue;
                    if (AytherSf2* sy = osy_for(sn.inst))
                        ayther_sf2_note_off(sy, 0, sn.key);
                    on.erase(on.begin() + static_cast<long>(i));
                }
                for (const AytherAudioEvent& e : audio_events) {
                    if (e.start_frame != f) continue;
                    if (e.pitch == 255) continue;          // DAC/ruido: sin nota
                    const auto ia = inst_assign.find(e.instrument);
                    if (ia == inst_assign.end()) continue;
                    if (event_muted(e)) continue;          // silenciado entero ()
                    // Prioridad ASSET > SF2 — mismo predicado que synth_tick:
                    // el mixdown debe sonar como el vivo.
                    if (event_covered_by_asset(e, f)) continue;
                    AytherSf2* sy = osy_for(e.instrument);
                    if (!sy) continue;
                    const int key = static_cast<int>(e.pitch) + ia->second.transpose;
                    if (key < 0 || key > 127) continue;
                    const int vel = e.velocity ? static_cast<int>(e.velocity) : 100;
                    for (size_t i = on.size(); i-- > 0;)
                        if (on[i].inst == e.instrument && on[i].key == key) {
                            ayther_sf2_note_off(sy, 0, key);
                            on.erase(on.begin() + static_cast<long>(i));
                        }
                    ayther_sf2_control(sy, 0, 7,
                        std::clamp(static_cast<int>(
                            (std::min)(ia->second.gain, 1.0f) * 127.0f + 0.5f), 0, 127));
                    ayther_sf2_note_on(sy, 0, key, vel);
                    on.push_back({ e.instrument,
                                   occ_key(e.chip, e.channel, e.start_frame),
                                   key, static_cast<int64_t>(e.end_frame),
                                   synth_one_shot(e.instrument) });
                }
            }
            flush_avail();
        }
        // Colita del resampler (media ventana de lookahead): offline no hay
        // próximo tick que la drene, se empuja un frame de silencio.
        blk.assign(static_cast<size_t>(router.mirror().rate() / fps + 1.0) * 2, 0.0f);
        rs.push(blk.data(), blk.size() / 2);
        flush_avail();

        for (auto& [_, sy] : osy)
            if (sy) ayther_sf2_free(sy);
        ayther_sf2_trim_cache();
        return out.size() >= 2;
    }

    /// : render offline AISLADO desde el espejo — el sustrato de los
    /// previews A/B y del WAV de eventos. A diferencia de export_router_base
    /// (que COMPONE lo que se entrega), acá se AÍSLA lo que se audita:
    ///
    ///   solo_mask   — canales enteros (el «Original, canal aislado» del A/B)
    ///   member_sigs — sólo las voces de esas firmas, con el modelo de 
    ///                 del lado de la FUENTE: ventana propia + cola de release
    ///                 que cede ante un evento ajeno del canal, y la exclusión
    ///                 del DAC ajeno en FM6 (S26 en «Melodía», 2026-07-23).
    ///
    /// El espejo da los 10 canales POR SEPARADO, así que aislar es sumar los
    /// que pasan el gate — lo que queda afuera es CERO digital, no un mute que
    /// deja pasar el residuo de . Sin SF2 ni sustituciones: el aislamiento
    /// se usa para escuchar el ORIGINAL de un canal o de un sonido.
    ///
    /// Devuelve false (→ el caller cae al camino viejo de captura) si no hay
    /// aislamiento pedido, el router está apagado o la toma no tiene cache.
    bool preview_render_base(uint32_t start, uint32_t win,
                             size_t max_frames, uint32_t solo_mask,
                             const std::vector<uint64_t>* member_sigs,
                             std::vector<int16_t>& out) {
        out.clear();
        const bool by_events = member_sigs && !member_sigs->empty() &&
                               !audio_events.empty();
        if (!by_events && !solo_mask) return false;   // mezcla completa: camino viejo
        if (!voice_router_on) return false;           // coherencia con lo que se oye
        // : el caché es incremental — un export más allá de lo
        // construido lo extiende (offline: desde el savestate propio si está
        // al final de lo construido, si no desde el inicio de la toma).
        voice_prime_ensure(start + win);
        if (voice_prime_capped || voice_prime_offsets.size() < 2) return false;
        const uint32_t cached = static_cast<uint32_t>(voice_prime_offsets.size() - 1);
        if (start >= cached) return false;
        const uint32_t last = (std::min)(start + win, cached);

        ChipMirror mirror;
        const double sfps = runner.fps();
        mirror.set_pal(sfps > 1.0 && sfps < 55.0);
        for (uint32_t i = 0; i < start; ++i) {
            const uint32_t a = voice_prime_offsets[i], b = voice_prime_offsets[i + 1];
            if (b > a) mirror.prime_frame(voice_prime_writes.data() + a, b - a);
        }
        StreamResampler rs;
        rs.set_rates(mirror.rate(), 44100.0);

        std::vector<float> blk, mixf;
        auto flush_avail = [&]() {
            const size_t avail = rs.available();
            if (!avail || out.size() / 2 >= max_frames) return;
            mixf.assign(avail * 2, 0.0f);
            rs.pull(mixf.data(), avail);
            const size_t room = max_frames * 2 - out.size();
            const size_t take = (std::min)(mixf.size(), room);
            for (size_t i = 0; i < take; ++i) {
                const int v = static_cast<int>(std::lrint(mixf[i] * 32767.0f));
                out.push_back(static_cast<int16_t>(
                    v > 32767 ? 32767 : (v < -32768 ? -32768 : v)));
            }
        };

        constexpr uint32_t kTail = 15;   // = kMuteTailFrames ()
        for (uint32_t f = start; f < last && out.size() / 2 < max_frames; ++f) {
            const uint32_t a = voice_prime_offsets[f], b = voice_prime_offsets[f + 1];
            mirror.run_frame(voice_prime_writes.data() + a, b - a);
            const size_t ns = mirror.frame_samples();

            uint32_t gate = solo_mask;
            if (by_events) {
                uint16_t own = 0, tail = 0, foreign = 0, fdac = 0;
                for (const AytherAudioEvent& e : audio_events) {
                    const uint32_t bit = chan_bit(e.chip, e.channel);
                    const bool member = std::find(member_sigs->begin(),
                                                  member_sigs->end(),
                                                  e.signature) != member_sigs->end();
                    if (member) {
                        if (f >= e.start_frame && f <= e.end_frame)          own  |= bit;
                        else if (f > e.end_frame && f <= e.end_frame + kTail) tail |= bit;
                    } else if (f >= e.start_frame && f <= e.end_frame) {
                        foreign |= bit;
                        // DAC ajeno: comparte la salida física de FM6 y PISA la
                        // nota — mientras suene, el canal queda afuera.
                        if (e.chip == 0 && e.channel == 5 && e.pitch > 127)
                            fdac |= bit;
                    }
                }
                gate = static_cast<uint16_t>((own | (tail & ~foreign)) & ~fdac);
            }

            blk.assign(ns * 2, 0.0f);
            if (gate) {
                for (int c = 0; c < ChipMirror::kChannels; ++c) {
                    if (!(gate & (1u << c))) continue;
                    const float* ch = mirror.channel(c);
                    for (size_t i = 0; i < ns * 2; ++i) blk[i] += ch[i];
                }
            }
            rs.push(blk.data(), ns);
            flush_avail();
        }
        // Colita del resampler: medio frame de lookahead — offline no hay
        // próximo tick que la drene.
        const double fps_drain = sfps > 1.0 ? sfps : 60.0;
        blk.assign(static_cast<size_t>(mirror.rate() / fps_drain + 1.0) * 2, 0.0f);
        rs.push(blk.data(), blk.size() / 2);
        flush_avail();
        // NO-VACUIDAD. El espejo puede devolver muestras y que sean TODAS CERO,
        // y hasta acá eso contaba como éxito: el caller se quedaba con el
        // silencio en vez de caer al core.
        //
        // Pasa cuando el span arranca al PRINCIPIO de la toma: el priming de
        // arriba (`for i < start`) no tiene frames que recorrer, así que el
        // mirror no hereda la configuración de instrumentos que el juego había
        // escrito ANTES de la grabación — la toma arranca de un savestate y
        // esas escrituras no están en ella. Los key-on del span suenan en vacío.
        //
        // Reporte 2026-08-06: la Secuencia «Opening» (frames 0-35 de Demo
        // Amazona) no sonaba ni en el preview ni en la reproducción; medido con
        // tools/seq_preview_probe, el espejo daba rms 0 y el CORE rms 1983 para
        // el mismo span. Devolver «no pude» hace caer al camino del core, que
        // sí tiene el audio. Esto no INVENTA sonido: un span mudo de verdad
        // sigue mudo por el core; lo que deja de haber es un éxito vacío.
        for (int16_t v : out)
            if (v != 0) return out.size() >= 2;
        out.clear();
        return false;
    }

    // -- LA PREGUNTA ÚNICA DEL MUTE () -----------------------------------
    //
    // Silenciar es silenciar. Hasta acá el mute y la SUSTITUCIÓN eran dos
    // caminos separados y sólo uno miraba el mute: la voz del chip callaba y el
    // asset HD (o el SoundFont) que la reemplaza seguía sonando — o sea que el
    // altavoz apagaba el original y dejaba el reemplazo, justo al revés de lo
    // que el artista pide. La causa de fondo era tener la decisión escrita tres
    // veces: en `voice_replaced` (chip), en los disparos de HD (que no la
    // tenían) y en `synth_tick` (sólo por instrumento).
    //
    // Estas tres funciones son ESA decisión, en un solo lugar, y las consumen
    // los cuatro caminos: el router, los disparos de HD, el sintetizador y el
    // mixdown del export.
    /// El bit de canal vive en el namespace del archivo (ver `chan_bit` arriba):
    /// lo usan tanto los métodos de Impl como las funciones libres de más abajo.
    /// La clave de ocurrencia del Lab (lab::audio_event_key).
    static uint64_t occ_key(uint8_t chip, uint8_t channel, uint32_t start) {
        return (static_cast<uint64_t>(chip) << 56) |
               (static_cast<uint64_t>(channel) << 48) | start;
    }

    /// ¿Hay ALGÚN mute puesto? Atajo para no pagar un escaneo en el caso
    /// común, que es el de siempre: nada silenciado.
    bool any_mute() const {
        return !audio_instrument_mute.empty() || !audio_occurrence_mute.empty() ||
               audio_manual_mute != 0;
    }

    /// ¿Está silenciada esta ocurrencia? Los tres ejes del panel Mezclar:
    /// el altavoz de un Sonido o de un Patrón (por instrumento), el ojo de una
    /// Secuencia (por ocurrencia) y el mute a mano del timeline (por canal).
    bool occurrence_muted(uint64_t occ, uint64_t inst) const {
        if (audio_instrument_mute.count(inst)) return true;
        if (!audio_occurrence_mute.empty() && audio_occurrence_mute.count(occ))
            return true;
        return (audio_manual_mute &
                chan_bit(static_cast<uint8_t>(occ >> 56),
                         static_cast<uint8_t>(occ >> 48))) != 0;
    }

    bool event_muted(const AytherAudioEvent& e) const {
        // : el bus del sonido calla la voz ORIGINAL igual que el altavoz
        // del artista. Es lo que hace que silenciar el bus de Música se lleve
        // también la música del juego, y no sólo su reemplazo HD.
        if (bus_muted_for(e.signature)) return true;
        if (!any_mute()) return false;
        return occurrence_muted(occ_key(e.chip, e.channel, e.start_frame),
                                e.instrument);
    }

    /// ¿Está silenciada esta Secuencia en la ventana anclada en `anchor`?
    ///
    /// El HD de una Secuencia es UNA mezcla de varias voces: no se puede
    /// silenciar a medias. Calla cuando NINGUNA de sus voces queda audible —
    /// que es exactamente lo que hacen los dos gestos del artista: el altavoz
    /// de un Patrón silencia todos sus timbres, y el ojo de la Secuencia
    /// silencia todas sus ocurrencias. Silenciar UN timbre de una Secuencia de
    /// varios no la calla, y eso es correcto: el asset sigue teniendo voces
    /// vivas que representar.
    bool seq_sub_muted(const AudioSeqSub& sq, uint32_t anchor) const {
        if (!any_mute()) return false;
        const uint32_t end =
            anchor + (sq.duration_frames ? sq.duration_frames : 1u);
        bool any_member = false;
        for (const auto& e : audio_events) {
            if (e.start_frame >= end) break;        // audio_events va en orden
            if (e.start_frame < anchor) continue;
            if (e.signature != sq.trigger_signature &&
                std::find(sq.signatures.begin(), sq.signatures.end(),
                          e.signature) == sq.signatures.end())
                continue;
            any_member = true;
            if (!event_muted(e)) return false;      // queda algo audible
        }
        return any_member;
    }

    /// Variante por FIRMA, para el camino EN VIVO (Capturar) y el del pack:
    /// ahí no hay toma analizada con la ocurrencia exacta, sólo la firma del
    /// canal que acaba de arrancar. Se resuelve contra los eventos analizados
    /// si los hay; sin ellos, sólo puede aplicar el mute por canal. `bit` es el
    /// canal donde suena (0 = no se sabe).  F3: si la voz trae su timbre
    /// (`instr`, del FFI de actives), el mute por instrumento se decide directo
    /// — una firma variante no está en los eventos analizados y antes escapaba.
    bool signature_muted(uint64_t sig, uint32_t bit, uint64_t instr = 0) const {
        // : el bus va ANTES del cortocircuito de `any_mute()`, que sólo
        // mira los mutes del ARTISTA (altavoz, ojo, canal). Un bus silenciado
        // es otra cosa —una decisión de mezcla, no de autoría— y si se
        // preguntara después, silenciar la música no haría nada en un proyecto
        // donde nadie tocó un altavoz.
        if (bus_muted_for(sig)) return true;
        if (!any_mute()) return false;
        if (bit && (audio_manual_mute & bit)) return true;
        if (audio_instrument_mute.empty()) return false;
        if (instr) return audio_instrument_mute.count(instr) != 0;
        for (const auto& e : audio_events)
            if (e.signature == sig)
                return audio_instrument_mute.count(e.instrument) != 0;
        return false;
    }

    /// La Secuencia EN VIVO. Sin toma analizada no hay ventana que mirar, así
    /// que la pregunta se hace sobre sus firmas miembro y su máscara de
    /// canales — mismo criterio que seq_sub_muted: calla cuando no le queda
    /// ninguna voz audible.
    bool seq_sub_muted_live(const AudioSeqSub& sq) const {
        if (!any_mute()) return false;
        if (sq.channel_mask &&
            (audio_manual_mute & sq.channel_mask) == sq.channel_mask) return true;
        if (audio_instrument_mute.empty() || sq.signatures.empty()) return false;
        for (uint64_t sig : sq.signatures)
            if (!signature_muted(sig, 0)) return false;
        return true;
    }

    /// Disparos de HD que NO sonaron por estar silenciados, y streams que hubo
    /// que CORTAR porque el artista silenció a mitad del asset. Sin esto, «el
    /// mute no funciona» y «el mute funciona pero el asset largo sigue hasta
    /// que termina» se ven idénticos desde afuera — y son arreglos distintos.
    uint64_t hd_muted = 0, hd_cut = 0;
    uint64_t hd_claimed = 0;   ///< : disparos reclamados por otra Secuencia (vivo)

    /// Bits de mute por OCURRENCIA (Secuencias deshabilitadas) en el frame f.
    /// La ventana se EXTIENDE kOccMuteTailFrames tras el end: el release
    /// FM/PSG sigue sonando tras el key-off y las colas se oían como una
    /// melodía fantasma «degradada» (reporte 2026-07-23). Excepción: un
    /// evento AJENO (no muteado) activo en el canal debe oírse.
    /// Cola de release del FM. La ventana de un evento va del key-on al
    /// key-off, pero el chip NO calla en el key-off: ahí ARRANCA el release. Un
    /// mute que cierra en `end_frame` destapa esa cola, y lo que se oye al
    /// reaparecer es un salto de amplitud — el «clic suave» del reporte del
    /// 2026-07-27.
    ///
    /// El 15 no es una corazonada: medido con tools/mute_silence_probe sobre
    /// Demo Barbaro, el residuo suelto cae de -29,8 a -42,2 dBFS al llegar a
    /// ~10 frames y de ahí se ESTANCA (60 y 120 frames no mejoran nada). Esa
    /// meseta es la cola; lo que queda después son eventos que el detector no
    /// vio, y alargar el mute no los arregla — sólo se comería audio bueno.
    static constexpr uint32_t kMuteTailFrames = 15;   // ~250 ms

    /// Máscara por-evento con cola de release. `want(e)` decide qué eventos se
    /// silencian; los demás DEFIENDEN su canal durante la cola.
    ///
    /// La ventana propia [start,end] se mutea siempre, igual que antes; sólo la
    /// COLA cede ante un evento ajeno. Así el arreglo es estrictamente aditivo
    /// —nunca desmutea algo que hoy se mutea— y no puede reabrir el «se oyen
    /// los dos superpuestos» que costó una escucha arreglar. Sin esa asimetría,
    /// extender la cola se llevaría puesto el golpe de espada que cae dos
    /// frames después de una nota de la música en el mismo canal FM.
    template <class Fn>
    uint32_t event_mute_with_tail(uint32_t f, Fn&& want) const {
        uint32_t own = 0, tail = 0, foreign = 0;
        for (const AytherAudioEvent& e : audio_events) {
            const uint32_t bit = chan_bit(e.chip, e.channel);
            if (want(e)) {
                if (f >= e.start_frame && f <= e.end_frame)          own  |= bit;
                else if (f > e.end_frame &&
                         f <= e.end_frame + kMuteTailFrames)         tail |= bit;
            } else if (f >= e.start_frame && f <= e.end_frame) {
                foreign |= bit;
            }
        }
        return own | (tail & ~foreign);
    }

    uint32_t occurrence_mute_at(uint32_t f) const {
        if (audio_occurrence_mute.empty()) return 0;
        // Éste aplica `foreign` a TODA la ventana, no sólo a la cola (ver
        // event_mute_with_tail): acá el mute es «esta ocurrencia no suena» y
        // otra aparición del mismo sonido debe seguir sonando. Semántica
        // distinta a propósito — por eso no comparte el helper.
        constexpr uint32_t kOccMuteTailFrames = kMuteTailFrames;
        uint32_t muted = 0, foreign = 0;
        for (const auto& e : audio_events) {
            const uint32_t bit = chan_bit(e.chip, e.channel);
            const uint64_t k = (static_cast<uint64_t>(e.chip) << 56) |
                               (static_cast<uint64_t>(e.channel) << 48) |
                               e.start_frame;
            if (audio_occurrence_mute.count(k)) {
                if (f >= e.start_frame && f <= e.end_frame + kOccMuteTailFrames)
                    muted |= bit;
            } else if (f >= e.start_frame && f <= e.end_frame) {
                foreign |= bit;
            }
        }
        return static_cast<uint32_t>(muted & ~foreign);
    }

    /// Ancla de la ventana de una sub de Secuencia en el frame f. Las
    /// ocurrencias de la firma disparadora se SEGMENTAN greedy: una que cae
    /// DENTRO de la ventana anclada por una anterior es INTERNA y NO ancla —
    /// la melodía repite su primera nota y re-anclar «en la más reciente»
    /// reiniciaba el HD ~1 s después del arranque (reporte 2026-07-23). Una
    /// repetición REAL (tras el fin de la ventana) sí re-ancla y re-dispara.
    /// audio_events va en orden de frame (el detector procesa secuencial).
    bool seq_sub_anchor(const AudioSeqSub& sq, uint32_t f,
                        uint32_t* anchor) {
        const auto& anchors = seq_anchors_of(sq);
        bool found = false;
        for (const uint32_t a : anchors) {
            if (a > f) break;                           // orden ascendente
            if (f < a + sq.duration_frames) {
                found   = true;   // el ancla MÁS RECIENTE que cubre f gana
                *anchor = a;
            }
        }
        return found;
    }

    /// : anclas de TODAS las subs en UNA pasada conjunta (segmentación
    /// greedy + RECLAMO entre Secuencias + prioridad en el empate) — el
    /// algoritmo vive en audio_seq_anchor.h (puro, con su oráculo). Caso
    /// real (reporte 2026-08-21, Golden Axe): «The Battle - Intro» y «- Loop»
    /// comparten 26 firmas y sonaban las dos a la vez.
    /// : las subs de Secuencia como las ve el anclaje (replay y vivo).
    std::vector<SeqAnchorSub> seq_anchor_view() const {
        std::vector<SeqAnchorSub> subs;
        subs.reserve(audio_seq_subs.size());
        for (const auto& s : audio_seq_subs) {
            SeqAnchorSub v;
            v.key = s.key; v.trigger_signature = s.trigger_signature;
            v.duration_frames = s.duration_frames; v.span_frames = s.span_frames;
            v.enabled = !s.asset.empty(); v.signatures = s.signatures;
            v.head = s.head_signatures; v.looping = s.looping;
            subs.push_back(std::move(v));
        }
        return subs;
    }
    const std::vector<uint32_t>& seq_anchors_of(const AudioSeqSub& sq) {
        if (seq_anchor_for_n != audio_events.size() ||
            seq_anchor_for_gen != audio_seq_subs_gen) {
            seq_anchor_for_n   = audio_events.size();
            seq_anchor_for_gen = audio_seq_subs_gen;
            const std::vector<SeqAnchorSub> subs = seq_anchor_view();
            seq_anchor_cache = seq_anchor_table(
                audio_events.size(),
                [&](size_t i) { return audio_events[i].signature; },
                [&](size_t i) { return audio_events[i].start_frame; }, subs);
        }
        static const std::vector<uint32_t> kNone;
        const auto it = seq_anchor_cache.find(sq.key);
        return it == seq_anchor_cache.end() ? kNone : it->second;
    }

    /// Prioridad ASSET > SF2 > original (reporte 2026-07-31): ¿en el frame f
    /// este evento lo está tocando un ASSET? Entonces el SoundFont NO lo
    /// sintetiza — antes sonaban superpuestos (el asset de la Secuencia y el
    /// SF2 del timbre a la vez). Cubre los dos caminos de asset: la asignación
    /// directa por firma y la membresía en una Secuencia con asset cuya
    /// ventana (seq_sub_anchor) cubre f. La voz original ya calla por el mute
    /// dinámico / buffer_router — este predicado sólo resuelve asset vs SF2.
    /// Consultado por synth_tick (vivo) Y por el mixdown offline: misma
    /// decisión en un solo lugar, el criterio de .
    /// : «lo toca un asset» = el asset está LISTO (hd_can_sound) — con el
    /// asset roto el SF2 sí sintetiza (prioridad ASSET > SF2 > original: el
    /// escalón que falla se saltea, no deja silencio). No-const por readiness.
    bool event_covered_by_asset(const AytherAudioEvent& e, uint32_t f) {
        //  F3: exacta o por regla — la ocurrencia variante también la
        // toca el asset (el disparo del replay resuelve con el mismo criterio).
        uint64_t asig = 0;
        if (resolve_event_sig(e.signature, e.instrument, e.pitch, &asig))
            if (const auto it = audio_event_assign.find(asig);
                it != audio_event_assign.end() &&
                // : una firma BLOQUEADA por sus condiciones no está
                // cubierta. Sin esto el mute seguía aplicándose y el resultado
                // era SILENCIO: ni el HD (gateado) ni el original (muteado).
                // La decisión de mute es única (), así que el gate tiene
                // que entrar acá y no sólo en el disparo.
                !audio_gated(asig) &&
                hd_can_sound(asig, it->second)) return true;
        for (const auto& sq : audio_seq_subs) {
            if (sq.asset.empty() || !hd_can_sound(sq.key, sq.asset)) continue;
            if (audio_gated(sq.key)) continue;   // : gateada = no cubre
            if (!sq.signatures.empty()) {
                if (std::find(sq.signatures.begin(), sq.signatures.end(),
                              e.signature) == sq.signatures.end())
                    continue;
            } else {
                // Subs viejas sin firmas: pertenencia por canal (mismo
                // fallback que el mute de produce_frame).
                const uint32_t bit = chan_bit(e.chip, e.channel);
                if (!(sq.channel_mask & bit)) continue;
            }
            uint32_t anchor = 0;
            if (seq_sub_anchor(sq, f, &anchor)) return true;
        }
        return false;
    }

    /// Máscara de mute dinámico COMPLETA para el frame f — lo MISMO que arma
    /// produce_frame (subs por evento y por Secuencia + instrumento +
    /// ocurrencia + manual) pero SIN disparos ni active_subs. Para los frames
    /// BARE del fast-forward, cuyo PCM se CONSERVA (catch-up del playback,
    /// ) y salía SIN mute: la Secuencia deshabilitada/sustituida se
    /// escuchaba A PLENO en cada ráfaga de deuda (reporte 2026-07-23).
    uint32_t dynamic_audio_mute_at(uint32_t f) {
        uint32_t mute = 0;
        auto chbit = [](const AytherAudioEvent& e) {
            return chan_bit(e.chip, e.channel);
        };
        // : el mute por sustitución exige el asset LISTO — un HD roto en
        // los frames bare del catch-up dejaría huecos de silencio que el
        // camino con produce ya no deja.
        if (audio_sub_preview && !audio_event_assign.empty())
            for (const auto& e : audio_events) {
                if (f < e.start_frame || f > e.end_frame) continue;
                //  F3: exacta o por regla (mismo criterio que el disparo).
                uint64_t asig = 0;
                if (!resolve_event_sig(e.signature, e.instrument, e.pitch,
                                       &asig)) continue;
                if (const auto it = audio_event_assign.find(asig);
                    it != audio_event_assign.end() &&
                    hd_can_sound(asig, it->second)) mute |= chbit(e);
            }
        if (audio_sub_preview)
            for (const auto& sq : audio_seq_subs) {
                if (sq.asset.empty() || !hd_can_sound(sq.key, sq.asset)) continue;
                uint32_t anchor = 0;
                if (!seq_sub_anchor(sq, f, &anchor)) continue;
                if (!sq.signatures.empty()) {
                    mute |= event_mute_with_tail(f, [&](const AytherAudioEvent& e) {
                        return std::find(sq.signatures.begin(), sq.signatures.end(),
                                         e.signature) != sq.signatures.end();
                    });
                } else {
                    mute |= sq.channel_mask;
                }
            }
        if (!audio_instrument_mute.empty())
            mute |= event_mute_with_tail(f, [&](const AytherAudioEvent& e) {
                return audio_instrument_mute.count(e.instrument) != 0;
            });
        // Un timbre RE-SINTETIZADO calla su voz original (): si no, se
        // oirían las dos a la vez. Se mutea por EVENTO y no por canal entero,
        // igual que el resto: en el Mega Drive los efectos comparten canales FM
        // con la música, y silenciar el canal completo se llevaría puesto un
        // golpe de espada que caiga en el medio.
        //
        // GATE DEL HD, igual que la Secuencia de arriba. Sin él, apagar los
        // Assets no cambiaba nada: la voz original seguía muteada y el timbre
        // seguía sonando. El modo Original tiene que devolver el juego intacto.
        if (synth_any && audio_sub_preview)
            mute |= event_mute_with_tail(f, [&](const AytherAudioEvent& e) {
                // DAC/ruido: el SF2 no lo cubre, así que su voz original queda.
                return e.pitch != 255 && inst_assign.count(e.instrument) != 0;
            });
        mute |= occurrence_mute_at(f);
        mute |= audio_manual_mute;
        return mute;
    }
    std::vector<AudioSeqSub>          audio_seq_subs;          // sustitución por Secuencia (grupo → 1 HD)
    std::unordered_set<std::string>   audio_prewarmed;         // assets HD ya decodificados (prewarm)
    // Disparos del preview YA hechos (clave → start_frame+1 de la ocurrencia;
    // 0 = nunca): el trigger dispara al ENTRAR a la ventana, no por igualdad
    // exacta con el start — el catch-up del playback (fast-forward ≤32 en
    // replay_seek) corre los intermedios en bare y si el start caía ahí el
    // one-shot se perdía («a veces no suena», 2026-07-23). Se limpian en los
    // saltos no-secuenciales (scrub) para poder re-disparar al re-pasar.
    std::unordered_map<uint64_t, uint32_t> audio_seq_fired;    // key de Secuencia
    std::unordered_map<uint64_t, uint32_t> audio_evt_fired;    // firma per-evento
    std::vector<AytherAudioActiveSub> audio_active_subs;
    uint32_t                          audio_mute_applied = 0;
    PlaneTileOccurrence    plane_tile_occs[kMaxPlaneTileOccs];   // Fase 2 (panel Capas)
    AytherTileSub          tile_subs[kMaxTileSubs];
    AytherSpriteSub        sprite_subs[kMaxSpriteOccs];
    uint8_t                sprite_sub_flips[kMaxSpriteOccs];  // CU-AN-11: flip observado por sub
                                                              // (paralelo; bit0 hflip, bit1 vflip)
    uint8_t                sprite_sub_tint[kMaxSpriteOccs * 3];  // E1 cromático: tinte RGB Q2.6 por sub (64 = 1.0)
    uint8_t                sprite_sub_slot[kMaxSpriteOccs];   // C8: slot SAT del sub (z-order; menor=encima)
    uint8_t                sprite_sub_prio[kMaxSpriteOccs];   // bit de prioridad VDP del sub (título GA: letras pri-1 DELANTE del plano A pri-1)
    // E1: peak-hold de luminancia por paleta (lo más brillante que estuvo cada
    // paleta en la sesión) — la referencia contra la que se normaliza el fade.
    // No se resetea en replay: durante un barrido de la toma acumula el máximo.
    double                 pal_luma_peak[4] = {0, 0, 0, 0};
    AytherSpriteSub        plane_tile_subs[kMaxPlaneTileSubs];   // Fase 2c (overlay HD de fondo)
    uint8_t                plane_tile_flips[kMaxPlaneTileSubs];  // paralelo: bit0 h, bit1 v
    uint8_t                plane_tile_sub_plane[kMaxPlaneTileSubs];  // paralelo: plano del sub (el enlace celda→set NO cruza planos)
    uint8_t                plane_tile_tint[kMaxPlaneTileSubs * 3];   // paralelo: tinte E1 Q2.6 (64 = neutro; sólo sets con ref autorada)
    PlaneCellHit           plane_cells[kMaxPlaneCells];          // celdas visibles (sync viewport↔Capas)
    /// R-3 (): celdas PARCIALES de los bordes (screen x/y negativos por
    /// scroll no alineado a 8). Van a un array LATERAL durante collect() y se
    /// publican como APÉNDICE de plane_cells recién después de la firma del
    /// Cuadro y los matchers (que iteran [0,npick) y no deben cambiar: las
    /// firmas ya autoradas dependen de ese conjunto exacto). Flag bit3 = parcial.
    PlaneCellHit           plane_cells_border[512];
    /// R-5 (): celdas consumidas por los matchers (paralelo a plane_cells
    /// [0,npick)) — insumo de SceneElement.claimed.
    std::vector<uint8_t>   plane_cell_claimed;
    /// R-5 (): la escena publicada en FrameView (siempre — el flag de
    /// convivencia se retiró con los canales de supresión).
    std::vector<SceneElement> scene_elements;
    // Oclusión de primer plano: máscara pantalla (1 byte/px) con los píxeles OPACOS
    // de tiles de plano de ALTA prioridad — el VDP los dibuja SOBRE los sprites de
    // prioridad baja (telones de transición, arcos). El compose la usa para tapar el
    // HD de pose igual que el hardware tapa al original. Inválida (w=h=0) si el
    // frame no tiene celdas hi-pri o el core no expone VRAM/VSRAM.
    std::vector<uint8_t>   plane_hi_opaque;
    uint16_t               plane_hi_w = 0, plane_hi_h = 0;
    AytherAudioSub         audio_subs[kMaxAudioOccs];
    /// : las subs por hash FILTRADAS por disponibilidad — play y mute
    /// consumen esta lista, así lo que no decodifica no silencia su batch.
    AytherAudioSub         audio_subs_ready[kMaxAudioOccs];
    // Scratch de claiming (pose-sets): qué occurrences reclamó un pose-set +
    // las restantes (sin reclamar) que alimentan el resolve per-sprite.
    uint8_t                sprite_claimed[kMaxSpriteOccs];
    AytherSpriteOccurrence sprite_occs_free[kMaxSpriteOccs];

    FrameView view;
    uint64_t  frame_index = 0;

    // Wire a freshly-opened pack into the script env + the three substitutors,
    // then autoload scripts/init.lua if the pack carries one.
    void load_pack_into(AyArchive* p) {
        // /: IDENTIDAD DEL HORNEADO, una vez y al principio del log.
        //
        // Con los assets nombrados por hash, un error dice `a3f9c1…` y no hay
        // forma de saber qué archivo es sin el log del bake que lo produjo. El
        // build id es lo que empareja las dos cosas: se deriva de integrity.toml
        // —el conjunto de hashes de todo lo que hay adentro— así que no puede
        // mentir, y dos horneados idénticos dan el mismo.
        //
        // Vacío = pack legacy (sin integrity.toml). Se dice así en vez de
        // callarlo: «no sé de qué build es» es información, y ocultarla mandaría
        // a buscar en un log que no existe.
        if (p) {
            const char* bid = ayther_pack_build_id(p);
            std::fprintf(stderr, "[pack] %s build %s\n",
                         ayther_pack_game_id(p),
                         (bid && *bid) ? bid : "(legacy, sin build id)");
        }
        // : la fuente PACK de la mejora por software se rearma desde cero
        // con cada pack (la fuente Lab, set_enhanced_elements, no se toca).
        for (auto& s : element_enhance_pack) s.clear();
        rebuild_enhance_sets();
        ayther_script_set_pack    (script.get(), p);
        ayther_tile_sub_load_pack (tile_sub.get(),   p);
        ayther_tile_sub_load_pack_named(plane_sub.get(), p, "plane_tile_substitutions.toml");
        ayther_sprite_sub_load_pack(sprite_sub.get(), p);
        ayther_pose_sub_load_pack(pose_sub.get(), p);   // CU-AN multi-sprite
        ayther_tween_load_pack(tween.get(), p);         // CU-AN in-betweens
        ayther_audio_sub_load_pack(audio_sub.get(),  p);

        const int64_t sz = ayther_pack_file_size(p, "scripts/init.lua");
        if (sz > 0) {
            std::vector<uint8_t> src(static_cast<size_t>(sz) + 1, 0);
            ayther_pack_read(p, "scripts/init.lua", src.data(), static_cast<size_t>(sz));
            ayther_script_load_string(
                script.get(), reinterpret_cast<const char*>(src.data()), "scripts/init.lua");
        }

        // Componentes: animations.toml (AnimationPlayer) + audio_events.toml
        // (mirror de asignaciones por evento; el core Rust parsea el mismo
        // archivo hacia el catálogo del sub de batches). define()/assign()
        // reemplazan por id → recargar el pack es idempotente y lo autorado
        // sin hornear sobrevive.
        auto read_text = [&](const char* name) -> std::string {
            const int64_t n = ayther_pack_file_size(p, name);
            if (n <= 0) return {};
            std::string s(static_cast<size_t>(n), '\0');
            ayther_pack_read(p, name, reinterpret_cast<uint8_t*>(s.data()), s.size());
            return s;
        };
        if (const std::string t = read_text("animations.toml"); !t.empty())
            parse_animations_toml(t, anim);
        if (const std::string t = read_text("audio_events.toml"); !t.empty())
            parse_audio_events_toml(t, audio_evt);

        //  EM-8.2: el gate del ENSANCHADO. Lo compila el CORE (camino A,
        // igual que el de audio): un segundo evaluador del dialecto de
        // condiciones se desincronizaría, y el autor tendría que aprender dos.
        //
        // Sin `widescreen.toml` el gate queda NULL y el ancho pedido manda tal
        // cual — que es lo que hace que todos los packs ya horneados y el
        // ensanchado manual del Lab sigan funcionando sin tocar nada.
        wide_gate.reset();
        if (const std::string t = read_text("widescreen.toml"); !t.empty())
            wide_gate.reset(ayther_widescreen_gate_new(t.c_str()));

        // : instruments.toml — la re-síntesis POR TIMBRE del pack. El
        // parser del core lo lee para otras cosas (soundfonts_used), pero
        // nadie lo traía a la sesión: en el Lab el catálogo llega del frontend
        // cada frame, y en Play NO LLEGA NADIE, así que la re-síntesis no
        // sonaba nunca. Mismo lector lineal naive que el resto de nuestros
        // TOML: el dialecto es el que escribe el proyecto, sin traducción.
        inst_assign_pack.clear();
        if (const std::string t = read_text("instruments.toml"); !t.empty()) {
            std::vector<PackInstrument> insts;
            parse_instruments_toml(t, insts);
            for (const PackInstrument& pi : insts) {
                InstAssign as;
                as.soundfont = pi.soundfont;
                as.bank      = pi.bank;
                as.preset    = pi.preset;
                as.transpose = pi.transpose;
                as.gain      = pi.gain;
                inst_assign_pack[pi.patch] = as;
            }
        }

        // : el Modo 3 viaja en el pack — game_profile.toml (anclas + gate,
        // derivado del modelo de Maper al hornear) + entity_substitutions.toml
        // (kind → asset HD por instancia, referenciado por nombre de entrada
        // del pack como los sprite subs). Un pack SIN perfil no toca el
        // resolver: lo que la autoría viva haya cargado/asignado sobrevive.
        // : los ACETATOS del pack. Se leen acá y se guardan; el stack lo
        // arma el frontend (ver pack_overlays() en el header). Mismo lector
        // lineal naive que el resto de nuestros TOML — el dialecto es el que
        // escribe el proyecto, sin traducción.
        overlays.clear();
        if (const std::string t = read_text("acetatos.toml"); !t.empty()) {
            auto quoted = [](const std::string& line) -> std::string {
                const size_t a = line.find('"');
                const size_t b = line.rfind('"');
                if (a == std::string::npos || b <= a) return {};
                return line.substr(a + 1, b - a - 1);
            };
            auto num = [](const std::string& line) -> double {
                const size_t eq = line.find('=');
                return eq == std::string::npos ? 0.0
                                               : std::atof(line.c_str() + eq + 1);
            };
            auto field = [](const std::string& line, const char* key) {
                // "opacity = 1" vs "opacity_x": exigir el separador evita que un
                // campo nuevo con prefijo común se coma al viejo.
                const size_t n = std::strlen(key);
                return line.compare(0, n, key) == 0 && line.size() > n &&
                       (line[n] == ' ' || line[n] == '=');
            };
            AytherSession::PackOverlay cur;
            bool open = false;
            size_t pos = 0;
            while (pos <= t.size()) {
                size_t nl = t.find('\n', pos);
                if (nl == std::string::npos) nl = t.size();
                std::string line = t.substr(pos, nl - pos);
                const bool eof = nl >= t.size();
                pos = nl + 1;
                while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                    line.pop_back();
                if (line.compare(0, 11, "[[acetato]]") == 0) {
                    if (open) overlays.push_back(std::move(cur));
                    cur = AytherSession::PackOverlay{};
                    open = true;
                } else if (!open) {
                    // comentario de cabecera
                } else if (field(line, "name"))    cur.name = quoted(line);
                else if (field(line, "visible"))
                    cur.visible = line.find("true") != std::string::npos;
                else if (field(line, "asset")) {
                    const std::string a = quoted(line);
                    std::snprintf(cur.content.asset, sizeof(cur.content.asset),
                                  "%s", a.c_str());
                } else if (field(line, "img_w"))
                    cur.content.img_w = uint16_t(num(line));
                else if (field(line, "img_h"))
                    cur.content.img_h = uint16_t(num(line));
                else if (field(line, "y"))
                    cur.content.y = int16_t(num(line));
                else if (field(line, "anchor"))
                    cur.content.anchor = uint8_t(num(line));
                else if (field(line, "factor"))
                    cur.content.factor = float(num(line));
                else if (field(line, "opacity"))
                    cur.content.opacity = float(num(line));
                else if (field(line, "blend"))
                    cur.content.blend = uint8_t(num(line));
                else if (field(line, "tile_mode"))
                    cur.content.tile_mode = uint8_t(num(line));
                else if (field(line, "drift_x"))
                    cur.content.drift_x = float(num(line));
                else if (field(line, "drift_y"))
                    cur.content.drift_y = float(num(line));
                // : lo que el lector IGNORABA. La estructura ya tenia los
                // campos —AytherLayerContent los declara todos— asi que lo
                // unico que faltaba era leerlos: se autoraban, se horneaban y
                // se perdian al abrir el pack.
                else if (field(line, "fit"))
                    cur.content.fit = uint8_t(num(line));
                else if (field(line, "flicker_amp"))
                    cur.content.flicker_amp = float(num(line));
                else if (field(line, "flicker_ticks"))
                    cur.content.flicker_ticks = uint16_t(num(line));
                else if (field(line, "pal"))
                    cur.content.pal_line = uint8_t(num(line));
                else if (field(line, "ref")) {
                    // "r,g,b" en decimal, como lo escribe el writer.
                    const std::string v = quoted(line);
                    int c[3] = {0, 0, 0};
                    std::sscanf(v.c_str(), "%d,%d,%d", &c[0], &c[1], &c[2]);
                    for (int k = 0; k < 3; ++k)
                        cur.content.ref_rgb[k] =
                            uint8_t(c[k] < 0 ? 0 : (c[k] > 255 ? 255 : c[k]));
                } else if (field(line, "tint_mask"))
                    cur.content.tint_mask = uint16_t(num(line));
                else if (field(line, "screen")) {
                    // Pipe-separated hexadecimal ids. When the fixed wire
                    // capacity is reached, retain the accepted prefix rather
                    // than disabling the gate completely.
                    const std::string v = quoted(line);
                    size_t b = 0;
                    while (b < v.size()) {
                        size_t e = v.find('|', b);
                        if (e == std::string::npos) e = v.size();
                        const std::string tok = v.substr(b, e - b);
                        const uint64_t id = tok.empty()
                            ? 0
                            : std::strtoull(tok.c_str(), nullptr, 0);
                        if (id != 0 && !cur.content.add_screen(id)) break;
                        b = e + 1;
                    }
                } else if (field(line, "gate"))
                    cur.content.gate_presence =
                        quoted(line) == "presencia" ? 1 : 0;
                else if (field(line, "frames")) {
                    // Pasos EXTRA de la animacion: el primero es el `asset`.
                    const std::string v = quoted(line);
                    size_t b = 0;
                    uint8_t k = 0;
                    while (b < v.size() && k < 3) {
                        size_t e = v.find('|', b);
                        if (e == std::string::npos) e = v.size();
                        const std::string tok = v.substr(b, e - b);
                        if (!tok.empty()) {
                            std::snprintf(cur.content.anim[k],
                                          sizeof(cur.content.anim[k]),
                                          "%s", tok.c_str());
                            ++k;
                        }
                        b = e + 1;
                    }
                    cur.content.anim_count = k;
                } else if (field(line, "ticks"))
                    cur.content.anim_ticks = uint16_t(num(line));
                if (eof) break;
            }
            if (open) overlays.push_back(std::move(cur));
        }

        if (const std::string t = read_text("game_profile.toml"); !t.empty()) {
            if (auto r = mode3.load_profile_str(t); !r) {
                std::fprintf(stderr, "[pack] game_profile.toml inválido: %s\n",
                             r.error.message.c_str());
            } else if (const std::string es =
                           read_text("entity_substitutions.toml");
                       !es.empty()) {
                // [[sub]] kind = "player" / asset = "<entrada del pack>" —
                // el mismo lector lineal naive de los demás TOML nuestros.
                auto quoted = [](const std::string& line) -> std::string {
                    const size_t a = line.find('"');
                    const size_t b = line.rfind('"');
                    if (a == std::string::npos || b <= a) return {};
                    return line.substr(a + 1, b - a - 1);
                };
                std::string kind;
                size_t      pos = 0;
                while (pos < es.size()) {
                    size_t nl = es.find('\n', pos);
                    if (nl == std::string::npos) nl = es.size();
                    const std::string line = es.substr(pos, nl - pos);
                    pos = nl + 1;
                    if (line.find("kind") == 0)
                        kind = quoted(line);
                    else if (line.find("asset") == 0 && !kind.empty()) {
                        mode3.assign_kind(kind, quoted(line));
                        kind.clear();
                    }
                }
            }
        }
        // : las Identidades autorables de Pintar —Cuadro, Panorámica,
        // Cinemática, Animación, Utilería, Carácter, UI— viajan en UN
        // documento. Antes eran cinco archivos separados por el MECANISMO del
        // motor que las sirve, que es un detalle de implementación decidiendo
        // el layout de la entrega.
        //
        // Un pack VIEJO trae los cinco por separado y se leen igual: el
        // documento único es la concatenación de los mismos arrays, así que los
        // decodificadores son los mismos y sólo cambia de dónde salen los bytes.
        std::vector<PackScreen>        pk_scr;
        std::vector<PackPanorama>      pk_pans;
        std::vector<PackKinematic>     pk_kins;
        std::vector<PackPlaneSequence> pk_seqs;
        std::vector<PackPlaneSet>      pk_sets;
        std::vector<PackPlaneFont>     pk_fonts;
        std::vector<PackEnhance>       pk_enh;     // 
        if (const std::string t = read_text("elements.toml"); !t.empty()) {
            parse_elements_toml(t, pk_scr, pk_pans, pk_kins, pk_seqs,
                                pk_sets, pk_fonts, &pk_enh);
        } else {
            if (const std::string t2 = read_text("plane_sets.toml"); !t2.empty())
                parse_plane_sets_toml(t2, pk_sets, pk_fonts);
            if (const std::string t2 = read_text("screens.toml"); !t2.empty())
                parse_screens_toml(t2, pk_scr);
            if (const std::string t2 = read_text("panoramas.toml"); !t2.empty())
                parse_panoramas_toml(t2, pk_pans);
            if (const std::string t2 = read_text("kinematics.toml"); !t2.empty())
                parse_kinematics_toml(t2, pk_kins);
            if (const std::string t2 = read_text("plane_sequences.toml"); !t2.empty())
                parse_plane_sequences_toml(t2, pk_seqs);
        }

        // : «Mejorar por software» — ya viene expandido a (capa, hash);
        // el inventario lo publica como fx_enhance y el compose lo aplica.
        // No pasa por el matcher ni por el resolver de assets.
        if (!pk_enh.empty()) {
            size_t n = 0;
            for (const PackEnhance& e : pk_enh) {
                if (e.layer > 3) continue;
                for (uint64_t h : e.hashes) { element_enhance_pack[e.layer][h] = e.k; ++n; }
            }
            rebuild_enhance_sets();
            std::fprintf(stderr, "[pack] mejora por software: %zu identidad(es), %zu (capa,hash)\n",
                         pk_enh.size(), n);
        }

        // Utilería (CU002) y Glifos (CU005): el catálogo multi-tile de Pintar.
        // Hasta que este archivo existió, los sets sólo vivían en la sesión de
        // autoría (inyectados por API) y el pack entregado NO reproducía
        // ninguna sustitución multi-tile de plano.
        {
            for (const PackPlaneSet& s : pk_sets) {
                Impl::PlaneSetDef d;
                d.plane   = s.plane;
                d.w_cells = s.w_cells;
                d.h_cells = s.h_cells;
                d.asset   = s.asset;
                std::memcpy(d.ref_rgb, s.ref_rgb, 3);   // tinte E1 (0,0,0 = sin)
                d.members.reserve(s.members.size());
                for (const PackPlaneSetMember& m : s.members)
                    d.members.push_back({ m.hash, m.cx, m.cy });
                d.off_x = s.off_x;   // 
                d.off_y = s.off_y;
                plane_sets[s.id] = std::move(d);   // por id → recargar es idempotente
            }
        }
        // CUADROS (CU001): pantallas estáticas completas.
        {
            for (const PackScreen& sc : pk_scr) {
                ScreenDef d;
                d.mask      = sc.plane_mask ? sc.plane_mask : 0x07;
                d.min_match = sc.min_match;
                d.max_extra = sc.max_extra;
                d.asset     = sc.asset;
                d.cells.reserve(sc.cells.size());
                for (const PackScreenCell& c : sc.cells) {
                    if (c.plane > 2 || !(d.mask & (1u << c.plane))) continue;
                    d.cells.emplace(((uint32_t)c.plane << 24)
                                    | ((uint32_t)c.col << 8) | c.row, c.hash);
                    uint64_t x = c.hash;
                    x ^= (uint64_t)c.col * 0x9E3779B97F4A7C15ull;
                    x ^= (uint64_t)c.row * 0xC2B2AE3D27D4EB4Full;
                    x ^= (uint64_t)(c.plane + 1) * 0x165667B19E3779F9ull;
                    x ^= x >> 33; x *= 0xFF51AFD7ED558CCDull; x ^= x >> 29;
                    d.sig_plane[c.plane] += x;      // por CAPA, no sumadas
                    ++d.cells_plane[c.plane];
                    d.hashes_plane[c.plane].insert(c.hash);
                }
                if (!d.cells.empty()) screens[sc.id] = std::move(d);
            }
        }

        // PANORÁMICAS (CU003): la tira del nivel de una capa. Sin esto, una
        // panorámica autorada vivía sólo en la sesión del Lab — el pack
        // entregado no la reproducía, que es el mismo agujero que tenían la
        // Utilería y los Glifos antes de plane_sets.toml.
        {
            for (const PackPanorama& p : pk_pans) {
                std::vector<AytherSession::PanoramaCell> cs;
                cs.reserve(p.cells.size());
                for (const PackPanoramaCell& c : p.cells)
                    cs.push_back({ c.hash, c.lx, c.ly });
                if (cs.empty()) continue;
                panoramas[p.id] = build_panorama(p.plane, p.origin_x, p.origin_y,
                                                 p.w_cells, p.h_cells,
                                                 cs.data(), (uint32_t)cs.size(),
                                                 p.asset);
            }
        }

        // CINEMÁTICAS (CU004): la secuencia ordenada de Cuadros. Va DESPUÉS de
        // screens.toml a propósito — sus pasos apuntan a esos ids.
        if (!pk_kins.empty()) {
            for (const PackKinematic& k : pk_kins) {
                if (k.steps.size() < 2) continue;   // un paso = un Cuadro
                KinematicDef d;
                d.gap   = k.gap_frames;
                d.loop      = k.loop;
                d.audio     = k.audio;
                d.gain      = k.gain;
                d.game_gain = k.game_gain;
                d.steps.reserve(k.steps.size());
                d.assets.reserve(k.steps.size());
                d.video_offsets.reserve(k.steps.size());
                for (const PackKinematicStep& s : k.steps) {
                    d.steps.push_back(s.screen_id);
                    d.assets.push_back(s.asset);
                    d.video_offsets.push_back(s.video_offset);
                }
                kinematics[k.id] = std::move(d);
            }
            kinematic_reindex();
            kinematic_reset();
        }

        // ANIMACIONES (): secuencias de Objetos con reloj propio. DESPUÉS
        // de plane_sets.toml — sus pasos apuntan a esos ids.
        {
            for (const PackPlaneSequence& q : pk_seqs) {
                if (q.steps.size() < 2) continue;
                PlaneSeqDef d;
                d.steps.reserve(q.steps.size());
                d.assets.reserve(q.steps.size());
                d.durs.reserve(q.steps.size());
                for (const PackPlaneSeqStep& s : q.steps) {
                    d.steps.push_back(s.set_id);
                    d.assets.push_back(s.asset);
                    d.durs.push_back(s.duration);
                    d.total += s.duration ? s.duration : kSeqDefaultDur;
                }
                plane_seqs[q.id] = std::move(d);
            }
            plane_seq_reindex();
            seq_clocks.clear();
        }

        // : y por último los timbres horneados. Va al FINAL para que
        // corra con el pack ya montado: `load_sf2_shared` busca el SF2 en el
        // pack antes que en disco.
        apply_pack_instruments();
    }

    /// : aplica los timbres de `instruments.toml` del pack.
    ///
    /// Sólo si NADIE autoró: el frontend manda su catálogo entero en cada
    /// vuelta (`set_instrument_assigns`), así que si el Lab tiene timbres
    /// asignados, esos ganan — mismo criterio que la unión Lab u pack de
    /// . En Play y en el runtime no hay quien mande, y ahí es donde esto
    /// hace la diferencia entre que la re-sintesis suene o no exista.
    void apply_pack_instruments() {
        if (inst_assign_pack.empty() || !inst_assign.empty()) return;
        for (const auto& [patch, as] : inst_assign_pack) {
            if (!synths.count(patch)) {
                AytherSf2* sy = load_sf2_shared(as.soundfont);
                if (!sy)
                    std::fprintf(stderr,
                        "[sf2] pack: no se pudo cargar '%s' — ese timbre "
                        "suena con su chip\n", as.soundfont.c_str());
                // Se cachea AUNQUE sea nulo, igual que en el camino del
                // frontend: sin esto se reintentaria por cada asignacion.
                synths[patch] = sy;
            }
            inst_assign[patch] = as;
            synth_any = true;
        }
        // Preset y REALCE por timbre, igual que set_instrument_assigns: la
        // atenuacion (<1) va por CC 7 en el note_on; el realce (>1) escala el
        // render de ESE sintetizador.
        for (auto& [inst, as] : inst_assign) {
            if (AytherSf2* sy = synth_for(inst)) {
                ayther_sf2_program(sy, 0, as.preset);
                if (as.gain > 1.0f) synth_boost[inst] = as.gain;
            }
        }
        ayther_sf2_trim_cache();
    }
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
AytherSession::AytherSession() : impl_(std::make_unique<Impl>()) {}
AytherSession::~AytherSession() = default;
AytherSession::AytherSession(AytherSession&&) noexcept            = default;
AytherSession& AytherSession::operator=(AytherSession&&) noexcept = default;

Result<std::unique_ptr<AytherSession>> AytherSession::create(const Config& cfg) {
    std::unique_ptr<AytherSession> session(new AytherSession());
    Impl& im = *session->impl_;

    // Hashers + script env (substitutors are created by set_pack below).
    im.tile_hasher.reset  (ayther_tile_hasher_new());
    im.sprite_hasher.reset(ayther_sprite_hasher_new());
    im.audio_hasher.reset (ayther_audio_hasher_new());
    im.audio_event_det.reset(ayther_audio_event_new());
    im.audio_live_det.reset (ayther_audio_event_new());
    im.script.reset       (ayther_script_new());

    // EM-7.1 (): las opciones del core, ANTES del init — el core las lee
    // durante `retro_load_game`, y después de eso ya no hay ventana. Ponerlas
    // más tarde compilaría y no haría nada, que es el peor resultado posible.
    for (const auto& [k, v] : cfg.core_options) im.runner.set_core_option(k, v);
    //  EM-7.4: el parche del usuario, antes de que el core lea la ROM.
    im.runner.set_patch_path(cfg.patch_path);

    // Emulator host — the only hard failure.
    if (!im.runner.init(cfg.core_path, cfg.rom_path)) {
        return Error{ ErrorCode::NotFound,
                      "RetroRunner::init failed (core or ROM): " + cfg.core_path };
    }
    im.core_path = cfg.core_path;   // : para instanciar el shadow core (lazy)
    im.rom_path  = cfg.rom_path;
    im.activate_ayther_subscriptions();   // E-2 ()

    // ABI 1.9 §5.1: qué hay del otro lado, dicho por el core y no deducido de
    // registros. Acá sólo lo que ya se sabe antes del primer frame (hardware y
    // región); el modo del VDP y el viewport se refrescan por frame en
    // refresh_abi_mirror() — al crear la sesión `vdp_mode` es 0 a propósito.
    if (im.runner.has_ayther_v1()) {
        im.sys_ok = im.runner.read_system_v1(im.sys).ok();
        if (im.sys_ok)
            std::fprintf(stdout, "[AytherSession] SYSTEM: hw=0x%02X %s lines=%u "
                         "(modo y viewport, por frame)\n",
                         im.sys.system_hw, im.sys.region_pal ? "PAL" : "NTSC",
                         im.sys.lines_per_frame);
    }

    // HD audio output — non-fatal (a missing device just means muted playback).
    if (cfg.enable_audio) {
        im.audio_enabled = im.audio.init();
        if (!im.audio_enabled)
            std::fprintf(stderr, "[AytherSession] audio init failed — continuing muted\n");
        // : el camino UNIFICADO es el ÚNICO. Pasó los oráculos (1×1 vs
        // catch-up byte-idéntico) y el A/B de oído del 2026-08-10 (Golden Axe,
        // attract completo: 66 voces HD con skew de colocación 0 sostenido).
        //  retiró el env `AYTHER_AUDIO_UNIFIED` y el camino de streams que
        // restituía: no quedaba nada que restituir.
    }

    // Wire emulator callbacks to the Impl (capture is stable: Impl is heap-pinned).
    Impl* ip = &im;
    im.runner.set_video_callback(
        [ip](const void* data, unsigned w, unsigned h, size_t pitch) {
            if (ip->cap_active) return;   // captura de audio: sin hashing de video
            if (!data || w == 0 || h == 0) return;
            // Compose (2º render B): capturar el snap para copiarlo, pero SIN hashear
            // (las occurrences/tiles ya se calcularon del frame A) ni publicar nada más.
            ayther_tile_hasher_process_frame(
                ip->tile_hasher.get(), static_cast<const uint8_t*>(data),
                w, h, pitch, ip->runner.pixel_format());
            ip->snap = { data, w, h, pitch };   // valid until end of this tick
        });
    im.runner.set_audio_callback(
        [ip](const int16_t* data, size_t frames) -> size_t {
            // Compose (2º render B): descartar el audio (el frame A ya lo emitió;
            // sin esto sonaría dos veces). No se hashea: B no recolecta nada.
            const uint64_t hash =
                ayther_audio_hasher_process_batch(ip->audio_hasher.get(), data, frames);
            if (ip->cap_active) {   // vista previa: acumular la MEZCLA de la ventana
                // No se filtra por hash: el audio del replay no es byte-reproducible
                // (la fase del FM diverge tras el load), así que aislar por hash no
                // sirve. Capturamos la mezcla real del momento → "en contexto".
                (void)hash;
                if (ip->cap_collect && data && frames)
                    ip->cap_pcm.insert(ip->cap_pcm.end(), data, data + frames * 2);
                return frames;      // sin passthrough al device durante la captura
            }
            if (ip->audio_enabled) ip->audio.buffer_emulator(hash, data, frames);
            return frames;
        });

    // Pack: explicit path, else derive "<core stem>.ay" (player convention,
    // disabled by the Lab so project sessions don't pick up stray dev packs).
    // Legacy: si no hay .ay al lado del core pero sí un .ae pre-rebrand, se usa.
    std::string pp = cfg.pack_path;
    if (pp.empty() && cfg.derive_core_pack) {
        const auto dot  = cfg.core_path.rfind('.');
        const auto stem = (dot != std::string::npos)
                            ? cfg.core_path.substr(0, dot) : cfg.core_path;
        pp = stem + ".ay";
        if (!std::filesystem::exists(pp) && std::filesystem::exists(stem + ".ae"))
            pp = stem + ".ae";
    }
    // A missing pack is not an error — set_pack returns ok and leaves has_pack()
    // false. Only a present-but-unopenable pack surfaces as an error, which we
    // tolerate at startup (run without HD assets).
    (void)session->set_pack(pp);

    // Router de canales por voz (): PUESTO POR DEFECTO desde el 2026-07-28.
    //
    // Estuvo detrás de un switch mientras el camino viejo era el de producción y
    // había que poder comparar sin recompilar. Ya no: el modelo sustractivo
    // —tapar el chip con una máscara derivada de VENTANAS— no se puede terminar
    // de arreglar, y sus dos fugas conocidas ( huecos entre nota y nota,
    //  la juntura entre Secuencias) no se corrigieron sino que dejaron de
    // poder ocurrir. Sostener los dos caminos era sostener también el que ya
    // sabemos que pierde.
    //
    // Lo que cuesta, medido con tools/fm_resynth_spike sobre Golden Axe:
    // correlación de envolvente 0,9906 (las mismas notas en el mismo lugar) y
    // 0,343 ms por frame de los 16,7 disponibles. A nivel de MUESTRA no nula —
    // dos emulaciones distintas del YM2612 nunca nulan— y por eso el veredicto
    // fue de oído, no del número.
    //
    // AYTHER_VOICE_ROUTER=0 restituye el camino viejo, entero, sin recompilar.
    // Es la salida de emergencia mientras el router sea nuevo; el día que sobre,
    // se retira junto con el camino viejo.
    {
        const char* v = std::getenv("AYTHER_VOICE_ROUTER");
        session->set_voice_router(!(v && v[0] == '0'));
    }
    if (const char* d = std::getenv("AYTHER_SF2_DUMP"))
        session->impl_->sf2_dump = std::fopen(d, "wb");
    if (const char* d = std::getenv("AYTHER_VOICE_DUMP")) {
        session->impl_->voice_dump = std::fopen(d, "wb");
        std::fprintf(stdout, "[voice] tee del router: %s (f32 estéreo crudo)\n",
                     session->impl_->voice_dump ? d : "NO PUDE ABRIR");
    }

    return session;
}

// ---------------------------------------------------------------------------
// Content: the HD pack (hot-reloadable)
// ---------------------------------------------------------------------------
Result<void> AytherSession::set_pack(const std::string& pack_path) {
    Impl& im = *impl_;
    namespace fs = std::filesystem;

    // Clean slate: detach the old pack from the script BEFORE closing it (no
    // dangling pointer), then recreate the substitutors to drop their catalogs.
    ayther_script_set_pack(im.script.get(), nullptr);
    im.tile_sub.reset  (ayther_tile_sub_new());
    im.plane_sub.reset (ayther_tile_sub_new());   // Fase 2c
    im.sprite_sub.reset(ayther_sprite_sub_new());
    im.pose_sub.reset  (ayther_pose_sub_new());   // CU-AN multi-sprite
    im.tween.reset     (ayther_tween_new());      // CU-AN in-betweens
    im.audio_sub.reset (ayther_audio_sub_new());
    // : los clips de video se quedan con una FUENTE que apunta al pack, no
    // con una copia de los bytes. Cerrar el pack sin tirar la cache dejaría
    // fuentes colgadas leyendo un AyArchive liberado — y el hotreload cierra y
    // reabre el pack en cada guardado del artista. Va ANTES del reset, como el
    // detach del script.
    im.videos.clear();
    // : pack nuevo = transacciones nuevas — los fallos de arranque del
    // pack viejo no aplican al contenido nuevo (hot-reload de Entregar).
    im.hd_failed_keys.clear();
    // : y la ESCALADA también. Un pack nuevo no hereda los fallos del
    // anterior; si no, hornear una corrección dejaría el subsistema apagado y
    // el autor pensaría que su arreglo no sirvió.
    im.escalation.clear();
    im.subsystems_on   |= im.auto_disabled_on;
    im.auto_disabled_on = 0;
    im.pack.reset();                 // close old pack (RAII)
    im.pack_path.clear();
    // : el perfil elegido era del pack VIEJO. Dos packs pueden tener un
    // «enhanced» cada uno y no ser el mismo — arrastrar la pista haría que el
    // pack nuevo se reportara en un perfil que nadie eligió para él.
    im.profile_hint.clear();

    if (pack_path.empty() || !fs::exists(pack_path))
        return Result<void>::ok();   // no pack — a valid state

    AyArchive* p = ayther_pack_open(pack_path.c_str());
    if (!p)
        return Result<void>::fail(ErrorCode::BadFormat,
                                  "pack exists but failed to open: " + pack_path);

    im.pack.reset(p);
    im.pack_path = pack_path;
    im.load_pack_into(p);
    // : el pack arranca en SU perfil predeterminado, no en el estado que
    // hubiera dejado el pack anterior. Sin esto, cargar un pack después de
    // haber apagado un subsistema a mano lo mostraría a medias y el autor
    // culparía al pack nuevo.
    //
    // Un pack sin `[[profile]]` declarado igual tiene uno («completo», derivado
    // de `[systems]`) cuya máscara es todo lo que trae — o sea, exactamente el
    // comportamiento de siempre. Esto no cambia nada para los packs que ya
    // existen.
    apply_default_profile();
    return Result<void>::ok();
}

Result<void> AytherSession::reload_pack() {
    if (impl_->pack_path.empty()) return Result<void>::ok();
    return set_pack(impl_->pack_path);
}

bool AytherSession::has_pack() const noexcept { return static_cast<bool>(impl_->pack); }

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
void AytherSession::set_input(int port, uint16_t buttons) noexcept {
    impl_->runner.set_input(port, buttons);
    if (port == 0) impl_->last_input0 = buttons;   // logged into the recording each step
}

// ---------------------------------------------------------------------------
// Frame stepping — the deterministic pipeline (motor-owned).
// ---------------------------------------------------------------------------
const FrameView& AytherSession::step() {
    Impl& im = *impl_;
    im.replay_pos = -1;   // el step en vivo mueve la máquina fuera del replay (R7d)
    // Rewind capture (R6): snapshot the PRE-frame state before advancing, so a
    // later pop() can restore exactly this frame. No-op (zero cost) when off.
    if (im.rewind.enabled() && im.runner.serialize(im.rewind_scratch))
        im.rewind.push(im.rewind_scratch);
    // Recording capture (R7): log the input that produces this frame.
    if (im.rec_active) im.rec_inputs.push_back(im.last_input0);
    ++im.frame_index;
    const FrameView& v = produce_frame();
    //  EM-7.3: los cheats del jugador, DESPUES de correr el frame.
    //
    // Lo intente al reves primero y el oraculo lo tumbo: escribir antes de
    // `produce_frame` no sirve de nada porque el juego pisa esas direcciones
    // DURANTE el frame — que es exactamente la razon por la que un cheat hay
    // que reaplicarlo. El valor tiene que quedar puesto cuando el juego ya
    // escribio, para que lo lea en el frame siguiente.
    //
    // Es lo mismo que hacen los emuladores con cheats, y la unica forma de
    // saberlo con certeza es probarlo contra algo que pise la direccion: el
    // core de prueba reescribe 0x100 en cada frame, y por eso el caso de la
    // suite de conformidad es el que decide esto y no un razonamiento.
    if (!im.cheats.empty()) {
        uint8_t* ram = im.runner.work_ram_mut();
        const size_t n = im.runner.work_ram_size();
        for (const Impl::CheatEntry& c : im.cheats) {
            // Los cheats vienen en direcciones del BUS 68k; work RAM empieza en
            // 0xFF0000. Un cheat al cartucho (ROM) no se puede aplicar aca: la
            // ROM es de solo lectura para nosotros, y fingir que se aplico
            // seria peor que ignorarlo.
            if (c.address < 0xFF0000u) continue;
            const uint32_t off = c.address - 0xFF0000u;
            if (!ram || off + 2 > n) continue;
            // Big-endian del bus, y con el `^1` del word-swap — el mismo camino
            // que `poke`, para que las direcciones que el jugador copio de una
            // revista sean las que documentan RetroAchievements.
            ram[(off) ^ 1u]     = (uint8_t)(c.value >> 8);
            ram[(off + 1) ^ 1u] = (uint8_t)(c.value & 0xFF);
        }
        im.poke_dirty = true;   // escribir fuera del input stream ensucia la sesion
    }

    // R7b: log this frame's occurrence summary for the timeline lanes.
    // R7c: log this frame's sprite hashes (CSR) for the per-hash presence lane.
    if (im.rec_active) {
        im.rec_stats.push_back({ static_cast<uint16_t>(v.sprite_occ_count),
                                 static_cast<uint16_t>(v.tile_occ_count),
                                 static_cast<uint16_t>(v.audio_occ_count),
                                 static_cast<uint16_t>(v.plane_a_count),
                                 static_cast<uint16_t>(v.plane_b_count),
                                 static_cast<uint16_t>(v.plane_w_count) });
        for (uint32_t i = 0; i < v.sprite_occ_count; ++i)
            im.rec_hashes.push_back(v.sprite_occs[i].hash);
        im.rec_hash_off.push_back(static_cast<uint32_t>(im.rec_hashes.size()));
        // .arp v7: hashes de audio de este frame (CSR) para las filas por-sonido.
        for (uint32_t i = 0; i < v.audio_occ_count; ++i)
            im.rec_audio_hashes.push_back(v.audio_occs[i].hash);
        im.rec_audio_off.push_back(static_cast<uint32_t>(im.rec_audio_hashes.size()));
        // Keyframe horneado (R7e): tras producir este frame, el estado da arranque
        // al SIGUIENTE frame de la toma (= rec_inputs.size()). Capturarlo en los
        // múltiplos de kReplayKeyInterval → seeks ≤ N frames sin re-simular desde 0.
        const uint32_t rframe = static_cast<uint32_t>(im.rec_inputs.size());
        if (rframe % kReplayKeyInterval == 0) {
            std::vector<uint8_t> st;
            if (im.runner.serialize(st) && !st.empty())
                im.rec_keyframes.emplace_back(rframe, std::move(st));
        }
    }
    return v;
}

// Run one frame from the current emulator state + build its FrameView. Shared
// by step() (forward) and rewind_step() (after restoring a past state). Does
// NOT capture rewind or touch frame_index — the callers own that.
namespace {
// Cobertura de un plano: # de celdas de la nametable con índice de tile != 0.
// La VRAM del fork viene word-swapped en LE (el byte lógico `off` vive en
// `off^1`, igual que la Work RAM) — leemos el word big-endian de la nametable
// con ese ^1. `base`/`wcells`/`hcells` salen de los VDP regs (mismas fórmulas
// que el tilemap viewer del Maper).
uint32_t plane_coverage(const uint8_t* vram, size_t vsz, uint32_t base,
                        int wcells, int hcells) {
    auto rd = [&](uint32_t off) -> uint32_t {
        const uint32_t i = off ^ 1u;
        return i < vsz ? vram[i] : 0u;
    };
    uint32_t covered = 0;
    const uint32_t cells = static_cast<uint32_t>(wcells) * static_cast<uint32_t>(hcells);
    for (uint32_t c = 0; c < cells; ++c) {
        const uint32_t nt = base + c * 2u;
        const uint16_t w  = static_cast<uint16_t>((rd(nt) << 8) | rd(nt + 1));
        if ((w & 0x7FFu) != 0) ++covered;
    }
    return covered;
}

// Enumera los tiles ÚNICOS (deduplicados por patrón+paleta) de un plano de la
// nametable, agregándolos a `out` desde `start` (acotado a `cap`). Identidad =
// FNV-1a del patrón (32 bytes 4bpp) + paleta → content-based, estable. Devuelve el
// nuevo total. `plane`: 0=A 1=B 2=Window. Word de nametable: P CC V H NNNNNNNNNNN.
/// Cataloga los tiles ÚNICOS (patrón+paleta) de un plano. `dropped` suma los
/// que no entraron por falta de cupo: antes el bucle simplemente cortaba y las
/// celdas restantes desaparecían EN SILENCIO. Tolerable mientras el catálogo
/// sólo alimentaba overlays selectivos; con la recomposición por elemento
/// (donde cada celda visible se dibuja desde este catálogo) un descarte mudo es
/// corrupción visible, así que ahora se cuenta y se avisa.
uint32_t collect_plane_tiles(const uint8_t* vram, size_t vsz, uint32_t base,
                             int wcells, int hcells, uint8_t plane,
                             PlaneTileOccurrence* out, uint32_t start, uint32_t cap,
                             uint32_t* dropped) {
    auto rd = [&](uint32_t off) -> uint32_t {
        const uint32_t i = off ^ 1u;
        return i < vsz ? vram[i] : 0u;
    };
    std::unordered_set<uint32_t> seen;            // key = pattern | (pal<<11), por plano
    uint32_t n = start;
    const uint32_t cells = static_cast<uint32_t>(wcells) * static_cast<uint32_t>(hcells);
    for (uint32_t c = 0; c < cells; ++c) {
        const uint32_t nt = base + c * 2u;
        const uint16_t w  = static_cast<uint16_t>((rd(nt) << 8) | rd(nt + 1));
        const uint16_t pattern = w & 0x7FFu;
        if (pattern == 0) continue;               // celda vacía
        const uint8_t  pal = (w >> 13) & 3u;
        const uint32_t key = pattern | (static_cast<uint32_t>(pal) << 11);
        if (!seen.insert(key).second) continue;   // patrón+paleta ya visto en este plano
        // Sin cupo: se sigue RECORRIENDO para contar cuántos quedaron afuera —
        // cortar acá escondía el problema y además sesgaba el catálogo hacia
        // las primeras filas de la nametable.
        if (n >= cap) { if (dropped) ++*dropped; continue; }
        uint64_t h = 1469598103934665603ULL;      // FNV-1a 64
        const uint32_t poff = static_cast<uint32_t>(pattern) * 32u;
        for (uint32_t b = 0; b < 32u; ++b) { h ^= rd(poff + b); h *= 1099511628211ULL; }
        h ^= pal; h *= 1099511628211ULL;
        PlaneTileOccurrence& o = out[n++];
        o.hash = h;
        o.cell_x = static_cast<uint16_t>(c % static_cast<uint32_t>(wcells));
        o.cell_y = static_cast<uint16_t>(c / static_cast<uint32_t>(wcells));
        o.pattern = pattern;
        o.plane = plane;
        o.palette = pal;
        o.hflip = (w >> 11) & 1u;
        o.vflip = (w >> 12) & 1u;
    }
    return n;
}
}  // namespace

// C8 v2 (2026-07-14): menor slot SAT de las occs reclamadas MIEMBRO de la pose
// del sub (centro dentro del rect + hash del pose-set). Alimenta el z-order
// entre HD reclamados Y el umbral "delante de la pose" del compose. 255 = sin
// miembro identificable (asset repetido, pose sin override).
static uint8_t sub_member_min_slot(
    const AytherSpriteSub& sb,
    const AytherSpriteOccurrence* occs, uint32_t n_occs, const uint8_t* claimed,
    const std::vector<AytherSession::PosePreview>& overrides) {
    const int rx = sb.screen_x, ry = sb.screen_y;
    const int rw = sb.w_px ? sb.w_px : sb.w_tiles * 8;
    const int rh = sb.h_px ? sb.h_px : sb.h_tiles * 8;
    uint8_t mn = 255;
    for (uint32_t o = 0; o < n_occs; ++o) {
        if (!claimed[o]) continue;
        const AytherSpriteOccurrence& oc = occs[o];
        const int cx = oc.screen_x + oc.w_tiles * 4;
        const int cy = oc.screen_y + oc.h_tiles * 4;
        if (cx < rx || cx >= rx + rw || cy < ry || cy >= ry + rh) continue;
        for (const auto& pv : overrides)
            if (pv.asset == sb.asset_path &&
                std::find(pv.hashes.begin(), pv.hashes.end(), oc.hash)
                    != pv.hashes.end()) {
                mn = std::min<uint8_t>(mn, oc.slot);
                break;
            }
    }
    return mn;
}

const FrameView& AytherSession::produce_frame() {
    Impl& im = *impl_;
    using clk = std::chrono::steady_clock;
    auto ms_since = [](clk::time_point t) {
        return std::chrono::duration<float, std::milli>(clk::now() - t).count();
    };

    im.snap = {};

    //  EM-8.2: el ancho EFECTIVO del frame. Se resuelve acá, junto con el
    // resto de los gates y con la misma RAM, para que el ancho no pueda quedar
    // desfasado un frame respecto de la condición que lo decide — un frame de
    // desfase en la transición a un menú es exactamente el «artefacto» que el
    // criterio de aceptación prohíbe.
    //
    // Sin gate el pedido manda tal cual: los packs ya horneados no declaran
    // nada y el ensanchado manual del Lab tiene que seguir andando.
    im.wide_w_eff = im.wide_w;
    if (im.wide_gate) {
        uint32_t w = 0;
        if (ayther_widescreen_gate_eval(
                im.wide_gate.get(), im.runner.work_ram(), im.runner.work_ram_size(),
                true, im.frame_index, &w))
            im.wide_w_eff = w;
    }

    // : los gates del compose por supresión (compose/preview_compose/
    // hide_compose) vivían acá en `constexpr false` desde R-5, junto al
    // pre-estado serializado que alimentaba el render B. Borrados con su
    // maquinaria.

    // R-5 parte 2b (): los canales de supresión del core MURIERON — la
    // sesión ya no escribe 0x102 (máscara de capas), 0x103 (sprites), 0x104
    // (tiles por celda) ni 0x105/0x106 (tiles de plano). La visibilidad vive
    // en el modelo de capas del renderer (AytherLayerStack + vdp_mask) y en
    // los elementos del inventario (SceneElement.hidden, que pliega TODOS los
    // ojos del Lab). El fb del emulador queda COMPLETO siempre: es la fuente
    // de verdad de los hashers, no el lienzo. El dim (0x108) sigue: es un
    // efecto del produce sobre el fb, y esos frames caen al blit (bit1).
    if (im.layer_dim_want) im.runner.set_layer_dim_v1(1);   // dim sólo en el frame visible
    // E-5 (): los tres resets manuales de contadores (sprites parseados,
    // raster dirty, escrituras de chip) se RETIRARON. El core los cierra y
    // renueva en el frame boundary de `capture_snapshot`, y escribir a mano un
    // contador que la ABI administra es pisarle el estado.
    // C-A3b: sustitución de audio por evento. Con el preview activo, mutear los
    // canales de los eventos ASIGNADOS cuya ventana [start,end] cubre este frame de
    // la toma, y reunir los subs activos (para que el playback dispare el asset HD
    // en sync). El mute es output-only (replay-safe); se aplica sólo a este frame
    // visible y se limpia abajo para la re-simulación bare.
    im.audio_active_subs.clear();
    uint32_t audio_mute = 0;
    if (im.audio_runtime_sub) {
        // Runtime EN VIVO: aplicar la máscara detectada el FRAME ANTERIOR (el key-on
        // de este frame se detecta recién tras run_frame → 1 frame de lag inherente).
        audio_mute = im.audio_runtime_mask;
    } else if (im.audio_sub_preview && !im.replay_quiet && !im.audio_event_assign.empty()) {
        // Tolerancia del disparo tardío: hasta el fast-forward máximo del
        // catch-up (los intermedios corren bare y no producen) — más atrás que
        // eso es un scrub al medio de un evento, no un disparo perdido.
        constexpr uint32_t kTriggerLateMax = 32;   // = kFastForwardMax
        const uint32_t f = static_cast<uint32_t>(im.frame_index);
        for (const auto& e : im.audio_events) {
            if (f < e.start_frame || f > e.end_frame) continue;
            //  F3: la ocurrencia resuelve exacta O por regla — `asig` es
            // la entrada AUTORADA (asset/tail/readiness); las keys de
            // ocurrencia (fired, streams, cortes) siguen con e.signature.
            uint64_t asig = e.signature;
            if (!im.resolve_event_sig(e.signature, e.instrument, e.pitch,
                                      &asig)) continue;
            const auto it = im.audio_event_assign.find(asig);
            if (it == im.audio_event_assign.end()) continue;
            // SILENCIADO (): el altavoz apaga el sonido ENTERO, no sólo su
            // voz original. Antes esta rama no miraba el mute y el asset HD
            // seguía sonando sobre un sonido «silenciado».
            const bool muted = im.event_muted(e);
            // : el original solo calla si el HD PUEDE sonar (o si el
            // artista lo silenció, que es mute con intención). Asset roto o
            // arranque fallido → la ventana no mutea y se oye el juego.
            const bool can = im.hd_can_sound(asig, it->second);
            if (muted || can)
                audio_mute |= chan_bit(e.chip, e.channel);
            im.audio_active_subs.push_back(AytherAudioActiveSub{
                e.signature, it->second.c_str(), e.chip, e.channel,
                static_cast<uint8_t>(f == e.start_frame ? 1u : 0u), 0u });
            // Disparo por ENTRADA al evento (no f == start exacto): si el start
            // cayó en un frame de catch-up (bare), el primer produce DENTRO del
            // evento dispara igual — una sola vez por ocurrencia (fired).
            uint32_t& fired = im.audio_evt_fired[e.signature];
            if (fired != e.start_frame + 1) {
                fired = e.start_frame + 1;
                // Solo REPRODUCIENDO (transport_playing): al scrubear en pausa
                // se marca sin sonar — el HD a velocidad normal con el cabezal
                // quieto no sigue al cabezal (reporte 2026-07-23). Silenciado
                // marca igual: desilenciar a mitad no lo arranca por el medio,
                // vuelve en la próxima ocurrencia.
                if (muted) ++im.hd_muted;
                else if (im.audio_enabled && im.transport_playing &&
                         f - e.start_frame <= kTriggerLateMax && !it->second.empty()) {
                    // : el resultado del disparo cierra la transacción —
                    // fallo = la key deja de mutear y suena el original.
                    // : con tail FINITO el one-shot entra al barrido y se
                    // corta en end_frame + tail (legacy = drena entero).
                    if (can) {
                        //  F3: tail y transacción por la entrada AUTORADA;
                        // el stream y su corte por la ocurrencia real.
                        const uint64_t cut =
                            im.cut_frame_of(asig, e.end_frame);
                        // : la ganancia del BUS multiplica a la autorada.
                        // Una asignación por firma suelta no tiene Secuencia y
                        // cae en Efectos (ver bus_of_signature).
                        if (im.hd_fired(asig,
                                im.audio.play_oneshot_asset_file(
                                    it->second, e.signature, 0.0,
                                    im.bus_gain_of(im.bus_of_signature(e.signature)))) &&
                            cut != UINT64_MAX)
                            im.hd_oneshot_cut[e.signature] = cut;
                    } else {
                        ++im.hd_fallback;
                    }
                }
            } else if (muted && im.audio_enabled) {
                // Silenciado A MITAD del asset. Sin esto el mute sólo evitaría
                // el próximo disparo y desde afuera se vería igual que «el mute
                // no funciona» — un asset largo seguiría hasta el final.
                // Los dos, siempre: el asset pudo haber salido por el one-shot
                // (asignación de autoría) o por el event-stream (pack).
                const bool cut_sfx = im.audio.stop_sfx_by_key(e.signature);
                const bool cut_evt = im.audio.stop_event(e.signature);
                if (cut_sfx || cut_evt) ++im.hd_cut;
            }
        }
    }
    // Sustitución por SECUENCIA (preview): cada Secuencia dispara sobre CUALQUIER
    // ocurrencia de su firma disparadora (trigger_signature) dentro de los eventos
    // detectados de la toma — no un rango [start,end] fijo por Secuencia — igual
    // que el mecanismo per-firma de arriba y que el pack exportado (pack_bake.cpp),
    // así el preview coincide con lo que hace el juego real y re-dispara en CADA
    // repetición, no sólo la primera.
    //
    // De las ocurrencias cuya ventana cubre el frame actual, se toma la MÁS
    // RECIENTE (mayor start_frame ≤ f), no la primera del array: si dos
    // repeticiones caen más cerca entre sí que `duration_frames` (p.ej. la
    // ventana de una está sobredimensionada porque el grupo abarca varias
    // ocurrencias), la ventana de la ocurrencia VIEJA seguiría "abierta" al
    // llegar la nueva y nunca se re-evaluaría — un evento reciente debe poder
    // cortar/reemplazar al anterior, igual que pasaría con el sonido real.
    if (im.audio_sub_preview && !im.replay_quiet && !im.audio_seq_subs.empty()) {
        const uint32_t f = static_cast<uint32_t>(im.frame_index);
        for (const auto& sq : im.audio_seq_subs) {
            if (sq.asset.empty()) continue;
            // Ventana anclada por SEGMENTACIÓN greedy de las ocurrencias del
            // disparador (seq_sub_anchor): una ocurrencia interna (la melodía
            // repite su primera nota) ya no re-ancla ni reinicia el HD.
            uint32_t best_start = 0;
            const bool found = im.seq_sub_anchor(sq, f, &best_start);
            if (found) {
                // Mute SELECTIVO (reporte 2026-07-23): dentro de la ventana se
                // silencian SOLO los eventos activos cuya firma es MIEMBRO de la
                // Secuencia (cada uno por su propio span), no el canal completo
                // — la música u otros SFX que compartan canal siguen sonando.
                // signatures vacío (subs viejas) = fallback al mask por canal.
                // Mute SELECTIVO con COLA DE RELEASE (, opción D). Sigue
                // silenciando SÓLO los eventos miembro —la música u otros SFX
                // que compartan canal siguen sonando— pero cada ventana se
                // extiende kMuteTailFrames.
                //
                // Sin la cola, entre el fin de una nota y el arranque de la
                // siguiente no hay evento activo y el chip se colaba. Medido en
                // «Melodía»: el canal 1 tenía 10 huecos de 7-14 frames, uno cada
                // ~1,92 s — el «golpe muy tenue cada 3-4 segundos» del reporte
                // del 2026-07-23.
                //
                // La cola cede ante un evento AJENO (ver event_mute_with_tail),
                // que es lo que hacía dudar de mutear el canal entero: un
                // espadazo que caiga dentro del pasaje no se pierde. En este
                // proyecto ese caso existe —«Espadazo de Tyris», canales 4 y 5,
                // frames 473-478, dentro del span de Melodía— así que no es
                // hipotético.
                //
                // SILENCIADA (): el HD de una Secuencia calla cuando no le
                // queda ninguna voz audible — silenciar el patrón entero, o
                // cerrarle el ojo, tiene que callar TAMBIÉN su reemplazo.
                const bool sq_muted = im.seq_sub_muted(sq, best_start);
                // : la ventana solo silencia el original si el HD puede
                // sonar (o si el artista la silenció). Asset roto = se oye la
                // música original entera, no un hueco.
                const bool sq_can = im.hd_can_sound(sq.key, sq.asset);
                if (sq_muted || sq_can) {
                    if (!sq.signatures.empty()) {
                        audio_mute |= im.event_mute_with_tail(
                            f, [&](const AytherAudioEvent& e) {
                                return std::find(sq.signatures.begin(),
                                                 sq.signatures.end(),
                                                 e.signature) != sq.signatures.end();
                            });
                    } else {
                        audio_mute |= sq.channel_mask;
                    }
                }
                // Disparo por ENTRADA a la ventana (no f == best_start exacto):
                // si el start cayó en un frame de catch-up (bare, sin produce),
                // el primer produce dentro de la ventana dispara igual — una
                // sola vez por ocurrencia (fired). Con retraso > catch-up máx
                // (scrub al medio de la ventana) marca sin sonar.
                constexpr uint32_t kSeqTriggerLateMax = 32;   // = kFastForwardMax
                uint32_t& fired = im.audio_seq_fired[sq.key];
                if (fired != best_start + 1) {
                    fired = best_start + 1;
                    // Solo REPRODUCIENDO — en pausa/scrub marca sin sonar.
                    // Entrada TARDÍA (> catch-up máx): el play arrancó DENTRO
                    // de la ventana (o un scrub reproduciendo cayó adentro) →
                    // disparar DESDE EL OFFSET, en sync con el cabezal
                    // (reporte 2026-07-23: «el audio asignado no arranca si la
                    // reproducción no comienza desde el comienzo»).
                    if (sq_muted) ++im.hd_muted;
                    else if (im.audio_enabled && im.transport_playing) {
                        const double fps = im.runner.fps() > 1.0
                                               ? im.runner.fps() : 60.0;
                        const double off = f - best_start <= kSeqTriggerLateMax
                                               ? 0.0
                                               : (f - best_start) / fps;
                        // : transacción — fallo = la ventana deja de
                        // mutear y suena el original.
                        if (sq_can) im.hd_fired(sq.key,
                                        im.audio.play_oneshot_asset_file(
                                            sq.asset.c_str(), sq.key, off,
                                            sq.gain * im.bus_gain_of(sq.bus)));   // 
                        else        ++im.hd_fallback;
                    }
                } else if (sq_muted && im.audio_enabled) {
                    // Silenciada A MITAD de la ventana: el asset de una
                    // Secuencia dura segundos, así que sin este corte el mute
                    // no se oiría hasta la próxima repetición — que es
                    // exactamente lo que se ve como «el mute no funciona».
                    if (im.audio.stop_sfx_by_key(sq.key)) ++im.hd_cut;
                }
                // CORTE por el JUEGO (reporte 2026-07-23; afinado con los 3
                // casos del reporte «Inicio» 2026-08-22): si la música de la
                // ventana TERMINÓ de verdad, el HD corta EN el fin real — no
                // un segundo después. El replay conoce el FUTURO de la toma
                // analizada, así que la pausa musical se distingue del final
                // sin gracia a ciegas:
                //   activo = un miembro suena en f (colchón kSeqCutLagFrames:
                //            el driver corta en un frame);
                //   retoma = un miembro ARRANCA en (f, f+gracia] → es una
                //            pausa de la música, no el final: no cortar.
                // Caso (1) sigue una sección hermana con firmas compartidas
                // (Intro→Loop): los miembros continúan → no corta; la ventana
                // y el reclamo deciden el traspaso. Caso (2) cambio de música
                // (el juego silencia todos los canales): ni activo ni retoma →
                // corte inmediato en el fin real. Caso (3) un SFX encima no
                // toca los miembros → no corta.
                //
                // Sólo DENTRO del span de EVENTOS: pasado el span, el silencio
                // es esperado — la cola del asset más largo que sus eventos
                // (Aliento de fuego: 42 frames de eventos, 154 de asset) es
                // AUTORÍA, no un corte del juego. (La gracia vieja de 60
                // frames se la comía: cortaba a los ~102 un asset de 154.)
                //
                // audio_events NO está ordenado por frame (lección ) —
                // pasada completa, sin break por «orden».
                constexpr uint32_t kSeqCutGraceFrames = 60;   // pausa musical máx (~1 s)
                constexpr uint32_t kSeqCutLagFrames   = 4;    // colchón sobre el fin real
                if (im.transport_playing && !sq.signatures.empty() &&
                    f > best_start + kSeqCutLagFrames &&
                    f < best_start + sq.span_frames) {
                    bool active = false, resumes = false;
                    for (const auto& e : im.audio_events) {
                        if (std::find(sq.signatures.begin(), sq.signatures.end(),
                                      e.signature) == sq.signatures.end())
                            continue;
                        if (e.start_frame <= f) {
                            if (e.end_frame + kSeqCutLagFrames >= f) {
                                active = true;
                                break;
                            }
                        } else if (e.start_frame <= f + kSeqCutGraceFrames) {
                            resumes = true;
                        }
                    }
                    if (!active && !resumes && im.audio_enabled)
                        // Telemetría: ESTE corte también cuenta (diagnóstico
                        // «Inicio»: se midió con hd_cut en 0 y pareció un
                        // drain temprano inexplicable).
                        if (im.audio.stop_sfx_by_key(sq.key)) ++im.hd_cut;
                }
            } else {
                // El cabezal SALIÓ de la ventana (scrub o pasó el final):
                // cortar el HD en el aire y re-armar el disparador — si el
                // cabezal vuelve a entrar reproduciendo, re-dispara como el
                // sonido real (reporte 2026-07-23: seguía sonando).
                if (auto it = im.audio_seq_fired.find(sq.key);
                    it != im.audio_seq_fired.end() && it->second != 0) {
                    if (im.audio_enabled) im.audio.stop_sfx_by_key(sq.key);
                    it->second = 0;
                }
            }
        }
    }
    // Mute DINÁMICO por INSTRUMENTO (panel Sonidos): a diferencia del mute manual
    // de abajo (por canal, incondicional), sólo mutea la ventana [start,end] de
    // cada evento cuyo instrument esté en el set — un instrumento que rota de
    // canal no mutea sonidos AJENOS que usan ese canal en otros tramos. Mismo
    // patrón/costo que el loop de sustitución por-firma de arriba.
    // La cola de release va acá también: medido, «Silenciar todos» dejaba el
    // residuo suelto en -29,8 dBFS y con la cola baja a -42,2 (mute_silence_probe
    // sobre Demo Barbaro) — eran los clics suaves del reporte del 2026-07-27.
    if (!im.audio_instrument_mute.empty()) {
        const uint32_t f = static_cast<uint32_t>(im.frame_index);
        audio_mute |= im.event_mute_with_tail(f, [&](const AytherAudioEvent& e) {
            return im.audio_instrument_mute.count(e.instrument) != 0;
        });
    }
    // Un timbre RE-SINTETIZADO calla su voz original (). Va ACÁ y no sólo
    // en `dynamic_audio_mute_at`, y eso costó una escucha: ESTE es el camino de
    // reproducción —arma la máscara EN LÍNEA— y aquella función sólo la usan el
    // export MP4 y el mixdown. Con el muteo puesto únicamente allá, se oían las
    // dos voces superpuestas.
    //
    // La lógica quedó duplicada en dos lugares que ya habían derivado; unificar
    // las dos máscaras es deuda anotada, no se toca en un arreglo de audio.
    if (im.synth_any && im.audio_sub_preview) {
        const uint32_t f = static_cast<uint32_t>(im.frame_index);
        audio_mute |= im.event_mute_with_tail(f, [&](const AytherAudioEvent& e) {
            // DAC/ruido: el SoundFont no aplica, su voz original se queda.
            return e.pitch != 255 && im.inst_assign.count(e.instrument) != 0;
        });
    }
    // Mute por OCURRENCIA (Secuencias deshabilitadas): ventana exacta + cola
    // de release, con override por evento ajeno (ver occurrence_mute_at) —
    // otras apariciones del mismo sonido siguen sonando.
    audio_mute |= im.occurrence_mute_at(static_cast<uint32_t>(im.frame_index));
    audio_mute |= im.audio_manual_mute;   // mute manual del timeline de Audios (siempre)
    // Con el router puesto y en CARTUCHO el chip NO SE MUTEA ACÁ. Todo lo que
    // se oye lo produce el router, y lo que impide que el chip suene es que el
    // router le ocupa el lugar en el staging del player (buffer_router) — no una
    // máscara. Ahí está el cambio de fondo: el mute deja de ser algo que hay que
    // acertar frame a frame (y que por eso dejaba pasar  y ).
    //
    // Muteando además acá, el chip callaba TAMBIÉN para el hasher de audio, que
    // se alimenta del PCM del core: las tomas salían sin hashes. El chip tiene
    // que seguir sonando para todo lo que lo OBSERVA y callar sólo para el que
    // lo ESCUCHA.
    //
    // En SEGA CD no se puede (). Ahí el buffer del core es el único portador
    // del chip PCM y del CDDA, que el router no espeja: ocupar su lugar dejaba
    // el sistema MUDO —medido, silencio absoluto contra -25,0 dBFS con el router
    // apagado—. El bloque se suma (router_mix) y lo que calla al original son
    // los diez canales que el router SÍ rinde. El costo aceptado es el que este
    // comentario describe: en Sega CD el hasher ve FM y PSG en silencio. Es un
    // camino legado —la identidad la da el detector de eventos— y no hay nada
    // autorado por hash en este hardware.
    if (im.voice_router_on)
        audio_mute = im.router_mix() ? (audio_mute | kRouterChannels) : 0;
    im.audio_mute_applied = audio_mute;
    im.runner.set_audio_mute_v1(audio_mute);

    // : ACÁ empieza el PCM de este frame dentro del bloque staged — los
    // disparos HD detectados tras run_frame se colocan en este offset, no al
    // principio del bloque (que en catch-up acumula varios frames).
    im.audio.mark_frame_boundary();
    im.runner.run_frame();          // fires the video + audio callbacks
    im.verify_ayther_subscriptions();      // E-2 (): una sola vez
    im.refresh_abi_mirror();               // E-3 (): VDP por la ABI, 1 vez/frame
    im.runner.set_audio_mute_v1(0);   // la re-sim bare corre sin mute
    // R-5: sin applies de supresión no hay restores — el core corre siempre
    // con el frame completo (los canales 0x102-0x106 quedaron inertes).
    if (im.layer_dim_want) im.runner.set_layer_dim_v1(0);   // la re-sim bare corre sin dim

    // -- Gather occurrences --------------------------------------------------
    const auto t_audio = clk::now();
    ayther_audio_hasher_end_tick(im.audio_hasher.get());

    const auto t_tile = clk::now();
    uint32_t n_tile_occs = 0;
    if (im.snap.data)
        n_tile_occs = ayther_tile_hasher_get_occurrences(
            im.tile_hasher.get(), im.tile_occs, kMaxTileOccs);
    const float tile_ms = ms_since(t_tile);

    const uint32_t n_audio_occs = ayther_audio_hasher_get_occurrences(
        im.audio_hasher.get(), im.audio_occs, kMaxAudioOccs);
    const float audio_ms = ms_since(t_audio);

    // Copiar el log de escrituras de chip del produce ANTES de cualquier re-sim
    // bare/compose (línea ~923 corre run_frame de nuevo y pisaría el buffer del
    // core). El layout de RetroRunner::AudioWrite es idéntico al AytherAudioWrite.
    {
        // E-3 (): dual-path. `ayther_audio_write_v1` y RetroRunner::AudioWrite
        // tienen el mismo layout (cycle u32, addr u16, data u8, chip u8), así que
        // el consumidor de abajo no distingue de dónde vinieron.
        AYTHER_LEGACY_READ_BEGIN
        const auto* aw = im.runner.audio_writes();
        uint32_t    naw = im.runner.audio_write_count();
        AYTHER_LEGACY_READ_END
        static_assert(sizeof(ayther_audio_write_v1) == sizeof(RetroRunner::AudioWrite),
                      "el layout de la escritura de chip tiene que coincidir");
        if (im.abi_snap_ok) {
            im.abi_audio.resize(im.abi_snap.audio_write_count);
            const auto ra = im.runner.read_audio_writes_v1(
                im.abi_audio.data(),
                static_cast<uint32_t>(im.abi_audio.size()), im.abi_snap);
            if (ra.ok()) {
                aw  = reinterpret_cast<const RetroRunner::AudioWrite*>(
                          im.abi_audio.data());
                naw = ra.count;
            } else if (ra.status == AYTHER_STATUS_NOT_SUBSCRIBED &&
                       !im.abi_audio_warned) {
                im.abi_audio_warned = true;
                std::fprintf(stderr,
                    "[AytherSession] AUDIO_WRITES sin suscripcion — "
                    "las escrituras siguen por el camino legacy\n");
            }
        }
        im.chip_writes.clear();
        if (aw && naw > 0) {
            static_assert(sizeof(RetroRunner::AudioWrite) == sizeof(AytherAudioWrite),
                          "AudioWrite ABI must match AytherAudioWrite");
            static_assert(sizeof(AytherAudioEvent) == 32,
                          "AytherAudioEvent ABI: 2×u64 + 2×u32 + 4×u8 (instrument incluido)");
            im.chip_writes.resize(naw);
            std::memcpy(im.chip_writes.data(), aw, static_cast<size_t>(naw) * sizeof(AytherAudioWrite));
        }
        // El router consume estas escrituras en el flush, DESPUÉS de las de los
        // frames bare del catch-up que ya están encoladas — el orden importa.
        im.voice_capture(im.chip_writes.data(), static_cast<uint32_t>(im.chip_writes.size()),
                         static_cast<uint32_t>(im.frame_index));

        // : el SEGUNDO camino del audio, en el mismo lugar y en el mismo
        // frame que el primero. El chip PCM de Sega CD no tiene bus expuesto,
        // así que no viaja como escritura cruda: el core lo entrega ya
        // tipificado. Se pollea acá —una vez por frame, un solo consumidor:
        // la cola es de consumo-al-leer— y se desempaqueta para que el detector
        // reciba los dos caminos en UNA llamada.
        im.pcm_events.clear();
        if (im.runner.ayther_api()) {
            static constexpr uint32_t kMaxAudioEvents = 4096;
            im.audio_evt_scratch.resize(kMaxAudioEvents);
            const uint32_t nev = im.runner.poll_audio_events_v1(
                im.audio_evt_scratch.data(), kMaxAudioEvents);
            for (uint32_t i = 0; i < nev; ++i) {
                const ayther_audio_event_v1& e = im.audio_evt_scratch[i];
                // Sólo el PCM. Los eventos de FM y PSG que vienen por acá son
                // los MISMOS hechos que ya llegan por las escrituras crudas, y
                // ésas son su fuente: consumirlos por los dos lados duplicaría
                // los key-on.
                if (e.source != AYTHER_AUDIO_SOURCE_PCM) continue;
                if (e.schema != AYTHER_LAYOUT_AUDIO_EVENT_V1) {
                    if (!im.pcm_schema_warned) {
                        im.pcm_schema_warned = true;
                        std::fprintf(stderr,
                            "[AytherSession] evento de PCM con schema %u; este "
                            "Engine lee %u — se ignoran\n",
                            e.schema, AYTHER_LAYOUT_AUDIO_EVENT_V1);
                    }
                    continue;
                }
                if (e.type != AYTHER_AUDIO_EVENT_NOTE_ON &&
                    e.type != AYTHER_AUDIO_EVENT_NOTE_OFF &&
                    e.type != AYTHER_AUDIO_EVENT_PITCH &&
                    e.type != AYTHER_AUDIO_EVENT_VOLUME) continue;
                // schema 2: reg = st | ls<<8 · data = fd | env<<16 | pan<<24
                // (en VOLUME, data = env | pan<<8). Ver ayther_api.h.
                AytherPcmEvent p{};
                p.kind    = e.type;   // mismos valores que AYTHER_PCM_*
                p.channel = e.channel;
                p.st      = static_cast<uint8_t>(e.reg & 0xFF);
                p.ls      = static_cast<uint16_t>((e.reg >> 8) & 0xFFFF);
                if (e.type == AYTHER_AUDIO_EVENT_VOLUME) {
                    p.env = static_cast<uint8_t>(e.data & 0xFF);
                    p.pan = static_cast<uint8_t>((e.data >> 8) & 0xFF);
                } else {
                    p.fd  = static_cast<uint16_t>(e.data & 0xFFFF);
                    p.env = static_cast<uint8_t>((e.data >> 16) & 0xFF);
                    p.pan = static_cast<uint8_t>((e.data >> 24) & 0xFF);
                }
                im.pcm_events.push_back(p);
            }
        }
    }

    //  F2: el detector live come SIEMPRE, como el espejo del router —
    // gameplay, replay, análisis y catch-up pasan todos por acá, así que su
    // shadow de registros y sus key-on siguen al RUNNER, no al workspace.
    // Activar la sustitución (entrar a Capturar) deja de ser un reset() ciego:
    // una nota sostenida ya está abierta con su firma correcta al entrar, y
    // los patches escritos hace minutos siguen en el shadow. El reset queda
    // solo para las transiciones REALES de sesión (reset/ROM). Tras un seek
    // por keyframe el unserialize no emite escrituras y el shadow queda
    // momentáneamente rancio — se cura cuando el driver reescribe; es el
    // mismo trato que recibe el espejo del router.
    if (im.audio_live_det) {
        // UNA llamada, los dos caminos (): escrituras crudas de FM/PSG y
        // eventos tipificados de PCM comparten el número de frame.
        ayther_audio_event_process_frame_ex(
            im.audio_live_det.get(), static_cast<uint32_t>(im.frame_index),
            im.chip_writes.data(), static_cast<uint32_t>(im.chip_writes.size()),
            im.pcm_events.data(), static_cast<uint32_t>(im.pcm_events.size()));
        //  F4: APRENDER firma→instrumento de los eventos CERRADOS antes
        // de tirarlos — es el único lugar donde el detector entrega el
        // instrumento en vivo. También fuera de Capturar: la identidad no
        // depende del workspace, y el análisis/replay enseñan las mismas
        // voces que después se resuelven en vivo.
        const uint32_t nev = ayther_audio_event_count(im.audio_live_det.get());
        if (nev > 0) {
            im.live_evt_scratch.resize(nev);
            ayther_audio_event_get(im.audio_live_det.get(),
                                   im.live_evt_scratch.data(), nev);
            for (const AytherAudioEvent& e : im.live_evt_scratch) {
                im.live_sig_instr[e.signature] =
                    Impl::LiveSigId{e.instrument, e.pitch};
                if (e.instrument && im.live_sig_covered(e.signature))
                    im.live_assigned_instr.insert(e.instrument);
            }
        }
        ayther_audio_event_clear_events(im.audio_live_det.get());  // no acumular histórico
    }

    // Sustitución de audio EN VIVO (runtime): ver qué canales están key-on, y
    // para los que tienen una firma ASIGNADA: muting (vía la máscara del próximo
    // frame — o la política del router por voz) + disparo del HD del que acaba
    // de encenderse. El mute/HD aplican desde el frame siguiente (lag de 1
    // frame inherente: las escrituras existen recién tras run_frame).
    if (im.audio_runtime_sub && im.audio_live_det) {
        AytherAudioActive act[64];
        const uint32_t na = ayther_audio_event_active(im.audio_live_det.get(), act, 64);
        // La política del router decide las voces de ESTE frame con esta foto
        // (live_voice_replaced) — el voice_tick corre más abajo en el mismo
        // produce, así que el key-on recién detectado ya la tiene.
        im.live_active.assign(act, act + na);
        // Ventanas de SECUENCIA vigentes (Mezclar): expirar las cumplidas y
        // acumular el mute. : si la firma trae MEMBERS, el mute es
        // SELECTIVO — solo los canales con un evento MIEMBRO activo este
        // frame (cada uno por su propio span); sin members (packs viejos)
        // cae al range-mute de la máscara de canales.
        uint32_t win_mask = 0;
        for (size_t k = 0; k < im.audio_seq_windows.size(); ) {
            if (im.frame_index > im.audio_seq_windows[k].end) {
                im.audio_seq_windows.erase(
                    im.audio_seq_windows.begin() + static_cast<std::ptrdiff_t>(k));
            } else {
                const Impl::SeqWindow& w = im.audio_seq_windows[k];
                // : si el HD de esta ventana ya no puede sonar (el arranque
                // falló tras abrirla, o el asset se rompió), la ventana deja de
                // silenciar el original — queda viva solo como bookkeeping.
                const auto wit = im.audio_event_assign.find(w.sig);
                if (wit == im.audio_event_assign.end() ||
                    !im.hd_can_sound(w.sig, wit->second)) { ++k; continue; }
                const auto mit = im.audio_event_members.find(w.sig);
                if (mit != im.audio_event_members.end() && !mit->second.empty()) {
                    for (uint32_t i = 0; i < na; ++i) {
                        const uint64_t asig = act[i].signature;
                        if (asig != w.sig &&
                            std::find(mit->second.begin(), mit->second.end(),
                                      asig) == mit->second.end())
                            continue;
                        win_mask |= chan_bit(act[i].chip, act[i].channel);
                    }
                } else {
                    win_mask |= w.mask;
                }
                ++k;
            }
        }
        uint32_t mask = 0;
        std::unordered_set<uint64_t> now;
        // : qué Secuencias del catálogo ANCLAN este frame — el MISMO
        // criterio que el replay y que Capturar (seq_anchor_frame): reclamo
        // entre Secuencias, cabeza por mayoría y continuación del loop. Las
        // firmas que arrancan = flanco de subida de las de interés (el
        // disparador, su cabeza, o una variante resuelta por regla → vale
        // como el disparador). Lo que no anclá acá no abre ventana abajo.
        std::unordered_set<uint64_t> anchor_now;
        {
            const std::vector<SeqAnchorSub> view = im.audio_event_seq_view();
            if (!view.empty()) {
                std::vector<SeqAnchorState> st(view.size());
                for (size_t k = 0; k < view.size(); ++k) {
                    if (const auto nx = im.audio_event_seq_next.find(view[k].key);
                        nx != im.audio_event_seq_next.end())
                        st[k].next_free = static_cast<uint32_t>(nx->second);
                    for (const auto& w : im.audio_seq_windows)
                        if (w.sig == view[k].key && im.frame_index <= w.end) {
                            st[k].open      = true;
                            st[k].win_start = static_cast<uint32_t>(w.start);
                            st[k].win_end   = static_cast<uint32_t>(w.end);
                        }
                }
                std::vector<uint64_t> sigs;
                for (uint32_t i = 0; i < na; ++i) {
                    const uint64_t sig = act[i].signature;
                    uint64_t asig = sig;
                    const bool resolved = im.resolve_event_sig(
                        sig, act[i].instrument, act[i].pitch, &asig);
                    bool interest = resolved && im.audio_event_duration.count(asig);
                    for (size_t k = 0; k < view.size() && !interest; ++k)
                        interest = std::find(view[k].head.begin(), view[k].head.end(), sig)
                                   != view[k].head.end();
                    if (!interest) continue;
                    now.insert(sig);
                    if (im.audio_live_prev.count(sig)) continue;   // no es flanco
                    if (std::find(sigs.begin(), sigs.end(), sig) == sigs.end()) sigs.push_back(sig);
                    if (resolved && asig != sig &&
                        std::find(sigs.begin(), sigs.end(), asig) == sigs.end())
                        sigs.push_back(asig);   // la variante vale como disparador
                }
                for (const size_t k : seq_anchor_frame(static_cast<uint32_t>(im.frame_index),
                                                       sigs, view, st)) {
                    anchor_now.insert(view[k].key);
                    im.audio_event_seq_next[view[k].key] = st[k].next_free;
                }
            }
        }
        // : abrir la ventana de la Secuencia `asig` + disparar su HD. `sig`
        // = la ocurrencia real (key de streams/instancias); con un anclaje por
        // CABEZA sin el disparador, sig = asig.
        std::unordered_set<uint64_t> opened_now;
        auto open_seq_window = [&](uint64_t asig, uint64_t sig, uint32_t ev_bit,
                                   const std::string& asset, uint32_t dur) {
            opened_now.insert(asig);
            uint32_t wmask = ev_bit;
            if (const auto ch = im.audio_event_channels.find(asig);
                ch != im.audio_event_channels.end() && ch->second)
                wmask = ch->second;
            const uint64_t wend = im.frame_index + dur;
            // La ventana guarda la firma AUTORADA: su mantenimiento
            // consulta el catálogo (assign/members/readiness).
            im.audio_seq_windows.push_back(
                Impl::SeqWindow{wend, wmask, asig, im.frame_index});
            // : con members el frame de apertura solo mutea el
            // bit del disparador (los demás miembros se evalúan por
            // actividad en los frames siguientes).
            win_mask |= im.audio_event_members.count(asig) ? ev_bit : wmask;
            const bool loop = [&] {
                const auto l = im.audio_event_looping.find(asig);
                return l != im.audio_event_looping.end() && l->second;
            }();
            const uint64_t cut = im.cut_frame_of(asig, wend);
            // : la instancia LÓGICA nace con la ventana, suene o
            // no (pausa/bypass) — reanudar la levanta con offset.
            im.audio_live_inst[sig] = Impl::LiveInstance{
                asset, im.frame_index, wend, cut, ev_bit, 1.0f, loop, false };
            // Solo REPRODUCIENDO () y sin bypass de Assets ().
            // : las condiciones del pack gatean el disparo. Bloqueada =
            // el HD no suena y el chip queda sonando: la degradacion correcta
            // es el ORIGINAL, no el silencio.
            if (im.audio_gated(sig)) return;
            if (im.transport_playing && !im.audio_live_bypass) {
                // : el resultado cierra la transacción. : el corte
                // (end + tail) viaja con el stream; el one-shot de disco
                // entra al barrido de hd_oneshot_cut.
                if (im.pack && ayther_pack_file_size(im.pack.get(), asset.c_str()) > 0) {
                    // : la ganancia AUTORADA de la Secuencia, multiplicada
                    // por la del BUS — el mismo contrato que ya aplica el
                    // one-shot de disco («la ganancia del BUS multiplica a la
                    // autorada», ). Antes el mixer recibia 1.0f fijo.
                    // : y la region de loop autorada — el mixer ya sabia
                    // ciclarla; lo que faltaba era que el dato llegara.
                    const auto lp = im.loop_of(sig);
                    im.hd_fired(asig, im.audio.play_event_hd(
                        im.pack.get(), asset.c_str(), loop, sig, wend, cut, 0.0,
                        im.fade_of(sig),   // 
                        im.gain_of(sig) *
                            im.bus_gain_of(im.bus_of_signature(sig)),
                        lp.first, lp.second));
                } else if (im.hd_fired(asig, im.audio.play_oneshot_asset_file(asset, sig)) &&
                           cut != UINT64_MAX) {
                    im.hd_oneshot_cut[sig] = cut;
                }
            }
        };
        for (uint32_t i = 0; i < na; ++i) {
            const uint64_t sig = act[i].signature;
            //  F3: la voz resuelve por firma exacta O por regla de
            // instrumento (la fragmentación medida en la transición
            // pantalla→demo: el mismo timbre en otra nota/canal es OTRA
            // firma). `asig` = entrada AUTORADA del catálogo (asset, ventana,
            // tail, readiness); `sig` = la ocurrencia real (flancos, keys de
            // streams e instancias — dos variantes simultáneas no se pisan).
            uint64_t asig = sig;
            if (!im.resolve_event_sig(sig, act[i].instrument, act[i].pitch,
                                      &asig)) continue;
            const auto it = im.audio_event_assign.find(asig);
            if (it == im.audio_event_assign.end()) continue;
            now.insert(sig);
            const uint32_t ev_bit = chan_bit(act[i].chip, act[i].channel);
            // SILENCIADO (). En vivo no hay toma analizada con la
            // ocurrencia exacta, así que la pregunta va por FIRMA: alcanza para
            // el altavoz de un Sonido y para el mute por canal, que son los que
            // pueden estar puestos acá. El mute del artista silencia el
            // original SIEMPRE (intención), esté el HD como esté.
            if (im.signature_muted(asig, ev_bit, act[i].instrument)) {
                mask |= ev_bit;
                if (!im.audio_live_prev.count(sig)) {
                    ++im.hd_muted;
                } else if (im.audio_enabled) {
                    const bool cut_sfx = im.audio.stop_sfx_by_key(sig);
                    const bool cut_evt = im.audio.stop_event(sig);
                    if (cut_sfx || cut_evt) ++im.hd_cut;
                }
                continue;
            }
            // : el original solo calla con el HD LISTO — asignación con
            // asset roto/ausente = el canal sigue sonando (fallback).
            const bool can = im.hd_can_sound(asig, it->second);
            if (can) mask |= ev_bit;
            if (im.audio_enabled && !im.audio_live_prev.count(sig)) {
                // Rising-edge. Firma con VENTANA (secuencia): abre la ventana
                // [f+1, f+duration] con la máscara de canales del catálogo
                // (fallback: el canal del evento) y dispara el HD — pack-aware
                // si el asset vive en el pack (runtime); disco en autoría.
                const auto d = im.audio_event_duration.find(asig);
                const uint32_t dur = d == im.audio_event_duration.end() ? 0u
                                                                        : d->second;
                // : la ventana la abre sólo quien ANCLÓ en la pre-pasada
                // (reclamada por otra Secuencia, interna al paso o sin
                // quórum = no); el original calla como miembro de la ventana
                // que la reclamó.
                if (dur > 0 && can && !anchor_now.count(asig)) ++im.hd_claimed;
                if (dur > 0 && can && anchor_now.count(asig) && !opened_now.count(asig)) {
                    open_seq_window(asig, sig, ev_bit, it->second, dur);
                } else if (can) {
                    // One-shot LIBRE (sin ventana): su fin es la duración del
                    // asset — la instancia lo registra igual (): reanudar
                    // a mitad lo levanta con offset; ya drenado, lo descarta.
                    im.audio_live_inst[sig] = Impl::LiveInstance{
                        it->second, im.frame_index, UINT64_MAX, UINT64_MAX,
                        ev_bit, 1.0f, false, false };
                    if (im.transport_playing && !im.audio_live_bypass)
                        im.hd_fired(asig, im.audio.play_oneshot_asset_file(
                            it->second, sig));
                } else if (!can && im.transport_playing && !im.audio_live_bypass) {
                    ++im.hd_fallback;   // suena el original de esta ocurrencia
                }
            }
        }
        // : anclajes por CABEZA cuyo disparador no arrancó (una variante):
        // abrir la ventana igual — el HD de la 3ª pasada del Loop.
        for (const uint64_t asig : anchor_now) {
            if (opened_now.count(asig)) continue;
            const auto it = im.audio_event_assign.find(asig);
            const auto d  = im.audio_event_duration.find(asig);
            if (it == im.audio_event_assign.end() || d == im.audio_event_duration.end() || !d->second)
                continue;
            if (!im.hd_can_sound(asig, it->second)) continue;
            open_seq_window(asig, asig, 0u, it->second, d->second);
        }
        // SECUENCIAS de autoría EN VIVO (Capturar): las subs de
        // set_audio_sequence_subs disparan acá por el flanco de subida de su
        // firma disparadora — el juego corre en DIRECTO (no hay toma analizada
        // que alinear), así que el anclaje es el key-on real que reporta el
        // detector. Mismo modelo que el pack exportado: range-mute de la unión
        // de canales de la Secuencia + su HD one-shot (de disco, con gain).
        uint32_t seq_mask = 0;
        {
            // : el MISMO criterio que el replay (seq_anchor_frame):
            // reclamo entre Secuencias, cabeza por mayoría y continuación del
            // loop. Las firmas que ARRANCAN este frame = flanco de subida de
            // las que interesan (disparador, cabeza, o la regla  F3 —
            // una variante del timbre vale como el disparador).
            const std::vector<SeqAnchorSub> view = im.seq_anchor_view();
            std::vector<SeqAnchorState>     st(view.size());
            for (size_t i = 0; i < view.size(); ++i) {
                if (const auto nx = im.audio_live_seq_next.find(view[i].key);
                    nx != im.audio_live_seq_next.end())
                    st[i].next_free = static_cast<uint32_t>(nx->second);
                for (const auto& w : im.audio_live_seq_win)
                    if (w.key == view[i].key) {
                        st[i].open      = true;
                        st[i].win_start = static_cast<uint32_t>(w.start_frame);
                        st[i].win_end   = static_cast<uint32_t>(w.end_frame);
                    }
            }
            std::vector<uint64_t> sigs;
            for (size_t i = 0; i < view.size(); ++i) {
                const AudioSeqSub& sq = im.audio_seq_subs[i];
                if (sq.asset.empty()) continue;
                for (uint32_t k = 0; k < na; ++k) {
                    const uint64_t sig = act[k].signature;
                    const bool exact = sig == sq.trigger_signature ||
                        std::find(sq.head_signatures.begin(), sq.head_signatures.end(), sig)
                            != sq.head_signatures.end();
                    bool rule = false;
                    if (!exact && sq.match_rule != AudioMatchRule::kExact &&
                        sq.match_instrument && act[k].instrument == sq.match_instrument)
                        rule = sq.match_rule != AudioMatchRule::kInstrumentPitch ||
                               (act[k].pitch != kAudioNoPitch && act[k].pitch == sq.match_pitch);
                    if (!exact && !rule) continue;
                    now.insert(sig);
                    if (im.audio_live_prev.count(sig)) continue;   // no es flanco
                    if (std::find(sigs.begin(), sigs.end(), sig) == sigs.end()) sigs.push_back(sig);
                    if (rule && std::find(sigs.begin(), sigs.end(), sq.trigger_signature) == sigs.end())
                        sigs.push_back(sq.trigger_signature);   // la variante vale como disparador
                }
            }
            for (const size_t i : seq_anchor_frame(static_cast<uint32_t>(im.frame_index),
                                                   sigs, view, st)) {
                const AudioSeqSub& sq = im.audio_seq_subs[i];
                // Segmentación por span (el paso del re-anclaje): la decide
                // seq_anchor_frame; acá sólo se persiste.
                im.audio_live_seq_next[sq.key] = st[i].next_free;
                // Re-anclar: la ventana vieja de esta Secuencia se reemplaza; el
                // one-shot con la MISMA key hace fade del disparo anterior.
                for (size_t k = 0; k < im.audio_live_seq_win.size(); )
                    if (im.audio_live_seq_win[k].key == sq.key)
                        im.audio_live_seq_win.erase(
                            im.audio_live_seq_win.begin() + static_cast<std::ptrdiff_t>(k));
                    else ++k;
                im.audio_live_seq_win.push_back(Impl::LiveSeqWin{
                    sq.key, im.frame_index + sq.duration_frames, im.frame_index,
                    sq.channel_mask, im.frame_index });
                // : instancia lógica de la Secuencia — vive con su ventana
                // (el corte por ausencia/expiración la cierra explícitamente).
                im.audio_live_inst[sq.key] = Impl::LiveInstance{
                    sq.asset, im.frame_index, im.frame_index + sq.duration_frames,
                    UINT64_MAX, 0, sq.gain, false, true };
                // SILENCIADA (): la ventana se abre igual —el range-mute del
                // original la necesita— pero el HD no suena.
                if (im.seq_sub_muted_live(sq)) ++im.hd_muted;
                else if (im.audio_enabled && im.transport_playing &&
                         !im.audio_live_bypass) {
                    // : transacción — asset no listo o arranque fallido = la
                    // ventana no acumula mute (abajo) y suena el original.
                    if (im.hd_can_sound(sq.key, sq.asset))
                        im.hd_fired(sq.key, im.audio.play_oneshot_asset_file(
                            sq.asset, sq.key, 0.0, sq.gain));
                    else
                        ++im.hd_fallback;
                }
            }
        }
        // Mantenimiento de las ventanas vivas: expirar, acumular el range-mute
        // y CORTAR POR AUSENCIA — ~1 s sin ningún evento miembro activo = el
        // juego cortó la música → el HD también (mismo criterio que el replay).
        constexpr uint64_t kLiveAbsenceFrames = 60;
        for (size_t k = 0; k < im.audio_live_seq_win.size(); ) {
            Impl::LiveSeqWin& w = im.audio_live_seq_win[k];
            const AudioSeqSub* sq = nullptr;
            for (const auto& s : im.audio_seq_subs)
                if (s.key == w.key) { sq = &s; break; }
            bool member_on = false;
            if (sq)
                for (uint32_t i = 0; i < na && !member_on; ++i)
                    member_on = act[i].signature == sq->trigger_signature ||
                                std::find(sq->signatures.begin(), sq->signatures.end(),
                                          act[i].signature) != sq->signatures.end() ||
                                //  F3: con regla, cualquier voz del timbre
                                // del disparador mantiene viva la ventana — los
                                // miembros variantes no están en la lista
                                // exacta y el corte por ausencia (~1 s) mataba
                                // el HD anclado por variante a mitad de camino.
                                (sq->match_rule != AudioMatchRule::kExact &&
                                 sq->match_instrument && act[i].instrument &&
                                 act[i].instrument == sq->match_instrument);
            if (member_on) w.last_seen = im.frame_index;
            const bool gone    = !sq;   // ojo cerrado / HD quitado (sub excluida)
            const bool expired = im.frame_index > w.end_frame;
            const bool absent  = im.frame_index - w.last_seen > kLiveAbsenceFrames;
            if (gone || expired || absent) {
                // : `expired` TAMBIÉN corta — la ventana es el contrato
                // audible de la Secuencia (dimensionada a su asset). Antes
                // expirar quitaba el mute pero dejaba el SFX drenando: original
                // restaurado + HD todavía sonando, los dos a la vez.
                if (im.audio_enabled) im.audio.stop_sfx_by_key(w.key);
                if (absent) im.audio_live_seq_next[w.key] = 0;  // la música puede volver
                im.audio_live_inst.erase(w.key);   // : cierre explícito
                im.audio_live_seq_win.erase(
                    im.audio_live_seq_win.begin() + static_cast<std::ptrdiff_t>(k));
            } else {
                // : la ventana solo silencia con su HD sano — silenciada
                // por el artista mutea igual (intención).
                if (im.seq_sub_muted_live(*sq) || im.hd_can_sound(w.key, sq->asset))
                    seq_mask |= w.mask;
                ++k;
            }
        }
        // : podar las instancias cuya ventana + tail quedó atrás — la
        // reanudación no debe levantar un reemplazo que el reloj ya pasó.
        // (El one-shot libre no entra acá: lo poda su duración al reanudar.)
        for (auto iit = im.audio_live_inst.begin();
             iit != im.audio_live_inst.end(); ) {
            const Impl::LiveInstance& li = iit->second;
            if (ayther::live_instance_over(im.frame_index, li.end_frame,
                                           li.cut_frame, li.looping))
                iit = im.audio_live_inst.erase(iit);
            else ++iit;
        }
        //  Fase 3: en bypass de Assets la máscara cae a 0 — suena el
        // juego original entero; el bookkeeping de arriba siguió igual.
        im.audio_runtime_mask =
            im.audio_live_bypass ? uint16_t(0)
                                 : uint32_t(mask | win_mask | seq_mask);
        im.audio_live_prev = std::move(now);
        // ( F4: el aprendizaje firma→instrumento de los cerrados vive
        // arriba, junto al process_frame incondicional — F2.)
        //  F4: clasificar las firmas ACTIVAS de este frame. `variant`
        // (sin match pero con el instrumento de una asignada) es la firma
        // FRAGMENTADA del issue — la que en la transición pantalla→demo se
        // cuela como original pese a que «ese sonido» está autorado.
        for (uint32_t i = 0; i < na; ++i) {
            const uint64_t sig = act[i].signature;
            if (im.live_sig_covered(sig)) { ++im.live_match_exact; continue; }
            uint64_t instr = act[i].instrument;   //  F3: viaja en el active
            if (!instr)
                if (const auto li = im.live_sig_instr.find(sig);
                    li != im.live_sig_instr.end()) instr = li->second.instrument;
            //  F3: resuelta por REGLA = match (cuenta aparte — mide cuánta
            // fragmentación cubrió la regla); ya no es variante ni unmatched.
            // Las reglas viven en el índice (per-firma) o en las Secuencias.
            bool rule_hit = im.audio_match_index.resolve(instr, act[i].pitch,
                                                         nullptr);
            for (size_t k = 0; !rule_hit && k < im.audio_seq_subs.size(); ++k) {
                const auto& sq = im.audio_seq_subs[k];
                if (sq.asset.empty() ||
                    sq.match_rule == AudioMatchRule::kExact ||
                    !sq.match_instrument || sq.match_instrument != instr)
                    continue;
                rule_hit = sq.match_rule != AudioMatchRule::kInstrumentPitch ||
                           (act[i].pitch != kAudioNoPitch &&
                            act[i].pitch == sq.match_pitch);
            }
            if (rule_hit) {
                ++im.live_match_rule;
                continue;
            }
            const bool variant = instr && im.live_assigned_instr.count(instr);
            if (variant) ++im.live_match_variant; else ++im.live_match_none;
            const auto ur = im.live_unmatched.find(sig);
            if (ur == im.live_unmatched.end()) {
                if (im.live_unmatched.size() < Impl::kLiveUnmatchedCap) {
                    Impl::LiveUnmatchedRec rec;
                    rec.instrument    = instr;
                    rec.first_frame   = im.frame_index;
                    rec.frames_active = 1;
                    rec.chip          = act[i].chip;
                    rec.channel       = act[i].channel;
                    rec.variant       = variant;
                    // El instrumento puede venir de una toma ANALIZADA aunque
                    // el detector live todavía no haya cerrado su primer
                    // evento (una vez por firma DISTINTA: barato).
                    if (!rec.instrument)
                        for (const auto& e : im.audio_events)
                            if (e.signature == sig) {
                                rec.instrument = e.instrument;
                                break;
                            }
                    im.live_unmatched.emplace(sig, rec);
                }
            } else {
                ++ur->second.frames_active;
                if (instr && !ur->second.instrument)
                    ur->second.instrument = instr;
                ur->second.variant = ur->second.variant || variant;
            }
        }
    }

    const auto t_sprite = clk::now();
    uint32_t n_sprite_occs = 0;
    if (im.sprite_hasher) {
        const uint8_t* vram = im.vram_ptr();          // E-3: espejo ABI o legacy
        const size_t   vsz  = im.runner.video_ram_size();
        if (vram && vsz > 0) {
            // Detección por SPRITES PARSEADOS: el fork captura en `parse_satb` los
            // sprites que el VDP realmente dibujó este frame (ids 0x10B/0x10C),
            // deduplicados. Es la fuente autoritativa de "qué hay en pantalla" y es
            // robusta a que el juego reescriba el SAT a mitad de frame o cambie su
            // base (el genio del logo Sega de Aladdin: el SAT a fin de frame muestra
            // solo placeholders, pero la lista parseada tiene al genio). Si la lista
            // viene vacía (core stock) → fallback al autodetect single-base por VRAM.
            // E-3 (): con ABI, la lista viene por read_region validada
            // contra la generación del snapshot; sin ABI, del puntero de
            // siempre. El hasher espera entradas de 10 bytes en los dos casos
            // — `ayther_sprite_v1` ES ese layout, así que no hay conversión.
            AYTHER_LEGACY_READ_BEGIN
            const uint8_t* psp = im.runner.parsed_sprites();
            uint32_t       pn  = im.runner.parsed_sprite_count();
            AYTHER_LEGACY_READ_END
            static_assert(sizeof(ayther_sprite_v1) == 10,
                          "el hasher lee entradas de 10 bytes");
            bool abi_sprites_ok = false;   // : la ABI respondió (aunque vacío)
            if (im.abi_snap_ok) {
                im.abi_sprites.resize(im.abi_snap.parsed_sprite_count);
                const auto rs = im.runner.read_parsed_sprites_v1(
                    im.abi_sprites.data(),
                    static_cast<uint32_t>(im.abi_sprites.size()), im.abi_snap);
                if (rs.ok()) {
                    psp = reinterpret_cast<const uint8_t*>(im.abi_sprites.data());
                    pn  = rs.count;
                    im.abi_sprite_count = rs.count;   // lo LEIDO, no el buffer
                    abi_sprites_ok      = true;
                } else if (rs.status == AYTHER_STATUS_NOT_SUBSCRIBED &&
                           !im.abi_sprites_warned) {
                    im.abi_sprites_warned = true;
                    std::fprintf(stderr,
                        "[AytherSession] SPRITE_CAPTURE sin suscripcion — "
                        "los sprites siguen por el camino legacy\n");
                }
            }
            // : CUANDO LA ABI CONTESTA, MANDA — aunque conteste "ninguno".
            // El fork publica los sprites que el VDP parseó en el frame; una
            // lista VACÍA significa que no dibujó ninguno (pantalla de mapa,
            // fundido), y eso es un dato, no una falla. Antes ese caso caía al
            // escaneo lineal del SAT, que devuelve lo que el juego dejó sin
            // limpiar: 60 sprites fantasma sobre el mapa de Golden Axe, con sus
            // parches y sus recortes negros. El fallback a VRAM queda para lo
            // que siempre fue suyo: cores sin la capacidad (stock).
            if (abi_sprites_ok || (psp && pn > 0)) {
                ayther_sprite_hasher_process_sprites(
                    im.sprite_hasher.get(), psp, pn, vram, vsz);
            } else {
                ayther_sprite_hasher_process_vram(
                    im.sprite_hasher.get(), vram, vsz, AYTHER_SAT_AUTODETECT);
            }
            n_sprite_occs = ayther_sprite_hasher_get_occurrences(
                im.sprite_hasher.get(), im.sprite_occs, kMaxSpriteOccs);
        } else if (!im.vram_warned) {
            im.vram_warned = true;
            std::fprintf(stderr,
                "[AytherSession] core exposes no VRAM (RETRO_MEMORY_VIDEO_RAM) — "
                "sprite detection disabled. Use a core build that exposes VRAM.\n");
        }
    }
    const float sprite_ms = ms_since(t_sprite);

    // -- Cobertura de planos A/B: nametables del VDP (regs 0x101) leídas en VRAM.
    // Métrica del timeline (lanes Plano A / Plano B). 0 si el core no las expone.
    uint32_t n_plane_a = 0, n_plane_b = 0, n_plane_w = 0;
    uint32_t n_plane_tiles = 0;   // elementos por tile de los 3 planos (panel Capas)
    uint32_t n_plane_tile_subs = 0;   // overlays HD de tiles de plano resueltos (Fase 2c)
    uint32_t n_plane_tile_hi   = 0;   // índice donde empiezan los de ALTA prioridad
    uint32_t n_plane_cells     = 0;   // celdas de plano visibles (sync viewport↔Capas)
    im.plane_hi_w = im.plane_hi_h = 0;   // máscara de oclusión hi-pri: se rearma en el pase scan(1)
    // Cámara del plano A para Modo 3 (capturada dentro del bloque Fase 2c, que ya
    // lee Hscroll/VSRAM). Sin VSRAM (core stock) queda en false → Modo 3 no-op.
    bool    m3_have_cam = false;
    int32_t m3_cam_x = 0, m3_cam_y = 0, m3_wpx = 0, m3_hpx = 0;
    int16_t  plane_hsc[3]      = {0, 0, 0};   // scroll whole-plane del frame (stitcher)
    int16_t  plane_vsc[3]      = {0, 0, 0};
    bool     vs_two_v          = false;       // : modo VS (vscroll por columna)
    int16_t  plane_vsc_col[2][20] = {};
    uint16_t plane_wpx_v       = 0;           // dims del plano A/B (módulo del wrap de scroll)
    uint16_t plane_hpx_v       = 0;
    uint32_t plane_occ_dropped = 0;           // tiles únicos sin cupo (nunca en silencio)
    uint64_t scr_sig[3]        = {0, 0, 0};   // firma de pantalla POR PLANO (Cuadro)
    uint32_t scr_cells[3]      = {0, 0, 0};
    {
        const uint8_t* vram = im.vram_ptr();          // E-3: espejo ABI o legacy
        const size_t   vsz  = im.runner.video_ram_size();
        const uint8_t* regs = im.regs_ptr();          // E-3
        const size_t   rsz  = im.runner.vdp_regs_size();
        if (vram && vsz > 0 && regs && rsz >= 0x20) {
            auto cells = [](int b) { return b == 1 ? 64 : (b == 3 ? 128 : 32); };
            const int wc = cells(regs[0x10] & 3);        // 32/64/128 (ya acotado)
            const int hc = cells((regs[0x10] >> 4) & 3);
            const uint32_t baseA = (uint32_t)(regs[0x02] & 0x38) << 10;
            const uint32_t baseB = (uint32_t)(regs[0x04] & 0x07) << 13;
            const uint32_t baseW = (uint32_t)(regs[0x03] & 0x3E) << 10;
            n_plane_a = plane_coverage(vram, vsz, baseA, wc, hc);
            n_plane_b = plane_coverage(vram, vsz, baseB, wc, hc);
            // Window (HUD): nametable en reg $3; tamaño fijo de pantalla (~64×32 H40).
            n_plane_w = plane_coverage(vram, vsz, baseW, 64, 32);
            // Elementos por tile (Fase 2): tiles únicos de cada plano para el árbol.
            n_plane_tiles = collect_plane_tiles(vram, vsz, baseA, wc, hc, 0,
                                                im.plane_tile_occs, n_plane_tiles,
                                                kMaxPlaneTileOccs, &plane_occ_dropped);
            n_plane_tiles = collect_plane_tiles(vram, vsz, baseB, wc, hc, 1,
                                                im.plane_tile_occs, n_plane_tiles,
                                                kMaxPlaneTileOccs, &plane_occ_dropped);
            n_plane_tiles = collect_plane_tiles(vram, vsz, baseW, 64, 32, 2,
                                                im.plane_tile_occs, n_plane_tiles,
                                                kMaxPlaneTileOccs, &plane_occ_dropped);
            if (plane_occ_dropped && !im.plane_occ_warned) {
                im.plane_occ_warned = true;
                std::fprintf(stderr,
                    "[planos] catalogo de tiles LLENO: %u tile(s) unico(s) fuera "
                    "de %u de cupo. Con overlays selectivos es tolerable; en una "
                    "recomposicion por elemento serian celdas sin dibujar.\n",
                    plane_occ_dropped, kMaxPlaneTileOccs);
            }
            // Fase 2b: re-armar la máscara de supresión por plano (id 0x105) con las
            // occurrences recién vistas: hash oculto → bit (plano, patrón, paleta).
            // Se aplica en el PRÓXIMO produce (produce-only, igual que tile_suppress);
            // en replay pausado converge al re-producir el mismo frame.
            if (im.plane_tiles_hidden.empty()) {
                im.plane_tile_suppress_any = false;
            } else {
                std::memset(im.plane_tile_suppress_want, 0, sizeof(im.plane_tile_suppress_want));
                bool any = false;
                for (uint32_t i = 0; i < n_plane_tiles; ++i) {
                    const PlaneTileOccurrence& o = im.plane_tile_occs[i];
                    if (o.plane > 2 || !im.plane_tiles_hidden.count(o.hash)) continue;
                    const uint32_t key = ((uint32_t)o.pattern << 2) | (o.palette & 3u);
                    im.plane_tile_suppress_want[o.plane * 1024u + (key >> 3)] |= (1u << (key & 7u));
                    any = true;
                }
                im.plane_tile_suppress_any = any;
            }

            // ── Fase 2c: resolver scroll-aware de overlays HD de tiles de plano ──
            // Para cada celda visible (Plano A/B) cuyo (plano,patrón,paleta) tiene un
            // asset asignado, emite un overlay 1×1 en su posición de pantalla. v2:
            // soporta flips (variante volteada del patrón, pre-flip en el renderer),
            // 2-cell vscroll, y PARTICIONA por prioridad (baja primero, alta después)
            // → el renderer dibuja la baja BAJO los sprites y la alta SOBRE ellos.
            // Reusa AytherSpriteSub. Necesita VSRAM (vscroll) — no-op con core stock.
            // EM-2 (): las condiciones del catálogo se evalúan UNA vez por
            // frame; después el lookup por celda sigue siendo O(1). No-op si el
            // pack no trae condiciones (todos los de hoy). La RAM va cruda con
            // word_swapped=true: el 68k llega con el addr^1 de libretro, y así
            // una dirección del TOML es la misma que se ve en RA/Data Crystal.
            ayther_tile_sub_begin_frame(
                im.plane_sub.get(), im.frame_index, im.runner.work_ram(),
                static_cast<uint32_t>(im.runner.work_ram_size()), true);
            ayther_tile_sub_clear_overrides(im.plane_sub.get());
            for (const auto& kv : im.lab_plane_overrides)
                ayther_tile_sub_add_override(im.plane_sub.get(), kv.first, kv.second.c_str());
            const uint8_t* vsram   = im.vsram_ptr();  // E-3
            const size_t   vsramsz = im.runner.vsram_size();
            if (vsram && vsramsz >= 4) {
                // (plano<<13 | patrón<<2 | paleta) → hash (de las occs ya deduplicadas:
                // la identidad de hash es idéntica a la del árbol Capas, sin re-hashear).
                std::unordered_map<uint32_t, uint64_t> key2hash;
                key2hash.reserve(n_plane_tiles);
                for (uint32_t i = 0; i < n_plane_tiles; ++i) {
                    const PlaneTileOccurrence& o = im.plane_tile_occs[i];
                    key2hash[((uint32_t)o.plane << 13) | ((uint32_t)o.pattern << 2) | o.palette] = o.hash;
                }
                auto rd16 = [&](uint32_t o) -> uint32_t {
                    return (o + 1 < vsz) ? (uint32_t)vram[o] | ((uint32_t)vram[o + 1] << 8) : 0u;
                };
                auto rd32 = [&](uint32_t o) -> uint32_t {
                    return (o + 3 < vsz) ? (uint32_t)vram[o] | ((uint32_t)vram[o+1] << 8)
                                         | ((uint32_t)vram[o+2] << 16) | ((uint32_t)vram[o+3] << 24) : 0u;
                };
                auto rdvs32 = [&](int col) -> uint32_t {
                    const size_t o = (size_t)col * 4u;
                    return (o + 3 < vsramsz) ? (uint32_t)vsram[o] | ((uint32_t)vsram[o+1] << 8)
                                            | ((uint32_t)vsram[o+2] << 16) | ((uint32_t)vsram[o+3] << 24) : 0u;
                };
                const uint32_t hscb  = ((uint32_t)regs[0x0D] << 10) & 0xFC00;
                const uint32_t hmaskTab[4] = { 0x00, 0x07, 0xF8, 0xFF };
                const uint32_t hmask    = hmaskTab[regs[0x0B] & 3];
                const bool     two_cell = (regs[0x0B] & 0x04) != 0;
                const int wpx = wc * 8, hpx = hc * 8;
                const int sw  = (int)im.snap.w, sh = (int)im.snap.h;
                const uint32_t vs0 = rdvs32(0);
                const int V0[2] = { (int)(vs0 & 0x3FF), (int)((vs0 >> 16) & 0x3FF) };  // A=low, B=high
                // Scroll whole-plane del frame (stitcher): H en la línea 0, V global.
                const uint32_t hw0 = rd32(hscb);
                plane_hsc[0] = (int16_t)(hw0 & 0x3FF);  plane_hsc[1] = (int16_t)((hw0 >> 16) & 0x3FF);
                plane_vsc[0] = (int16_t)V0[0];          plane_vsc[1] = (int16_t)V0[1];
                plane_wpx_v  = (uint16_t)wpx;           plane_hpx_v  = (uint16_t)hpx;
                // : vscroll POR COLUMNA al FrameView. En modo whole-plane
                // las 20 columnas replican el global (consumidores sin chequeo
                // del flag ven lo mismo de siempre).
                vs_two_v = two_cell;
                for (int c2c = 0; c2c < 20; ++c2c) {
                    int VA = V0[0], VB = V0[1];
                    if (two_cell) {
                        const uint32_t vc = rdvs32(c2c);
                        VA = (int)(vc & 0x3FF); VB = (int)((vc >> 16) & 0x3FF);
                    }
                    plane_vsc_col[0][c2c] = (int16_t)VA;
                    plane_vsc_col[1][c2c] = (int16_t)VB;
                }
                const uint32_t base[2] = { baseA, baseB };
                // Window (plano 2): screen-aligned (sin scroll), pero sólo dentro de su
                // región de clip (reg[17]/[18]); fuera de ella se ve Plano A. Dims como
                // collect_plane_tiles (64×32) para que el hash del árbol coincida.
                // Cámara absoluta del plano A para Modo 3 (mismo camino validado por
                // mode3_spike): H de la línea 0 de la tabla Hscroll, V de la col 0 de
                // VSRAM; cam_x ≡ −H (mod wpx), cam_y ≡ +V (mod hpx). Parado, coincide
                // EXACTO con Camera_X/Y_pos de la work RAM (riesgo  validado).
                {
                    const uint32_t hw0 = rd32(hscb);   // línea 0: A = word bajo, B = alto
                    const int HA = (int)(hw0 & 0x3FF), HB = (int)((hw0 >> 16) & 0x3FF);
                    m3_cam_x = ((-HA) % wpx + wpx) % wpx;
                    m3_cam_y = ((V0[0]) % hpx + hpx) % hpx;
                    m3_wpx = wpx; m3_hpx = hpx;
                    m3_have_cam = true;

                    // ── Fondos (Componentes): acumular las celdas visibles de A/B en
                    // ESPACIO DE NIVEL — misma mecánica que tools/background_spike: unwrap
                    // del scroll wrapeado → cámara absoluta; ventana visible → celda de
                    // nivel → observe (el stitcher dedupe y clasifica animados).
                    if (im.bg_capture_on && !im.bg_scene_cut && im.bg_st) {
                        if (!im.bg_uxA) {                 // lazy: necesita wpx/hpx
                            im.bg_uxA.reset(ayther_scroll_unwrapper_new(wpx));
                            im.bg_uyA.reset(ayther_scroll_unwrapper_new(hpx));
                            im.bg_uxB.reset(ayther_scroll_unwrapper_new(wpx));
                            im.bg_uyB.reset(ayther_scroll_unwrapper_new(hpx));
                        }
                        const int camxB = ((-HB) % wpx + wpx) % wpx;
                        // : V de B = VSRAM impar (V0[1]) con unwrap PROPIO — con el
                        // de A el stitcher no veía la subida de GA (vscB 126→105 con
                        // vscA=0, verificado con pano_sweep_probe).
                        const int camyB = ((V0[1]) % hpx + hpx) % hpx;
                        const int64_t absxA = ayther_scroll_unwrapper_push(im.bg_uxA.get(), m3_cam_x);
                        const int64_t absyA = ayther_scroll_unwrapper_push(im.bg_uyA.get(), m3_cam_y);
                        const int64_t absxB = ayther_scroll_unwrapper_push(im.bg_uxB.get(), camxB);
                        const int64_t absyB = ayther_scroll_unwrapper_push(im.bg_uyB.get(), camyB);

                        // : CORTE DE ESCENA — una cámara no salta más de
                        // ~32 px en un frame (Sonic a tope ≈ 16); un delta
                        // mayor = cambio de escena (título/intro resetean el
                        // scroll) y el unwrap seguiría acumulando la escena
                        // nueva SOBRE el espacio del nivel (celdas del título
                        // pegadas en la tira, pasada 2026-07-15). Congelar el
                        // stitch conserva lo bueno; re-armar = reiniciar la
                        // captura en Fondos.
                        constexpr int kBgMaxStepPx = 32;
                        const int steps[4] = {
                            ayther_scroll_unwrapper_last_step(im.bg_uxA.get()),
                            ayther_scroll_unwrapper_last_step(im.bg_uyA.get()),
                            ayther_scroll_unwrapper_last_step(im.bg_uxB.get()),
                            ayther_scroll_unwrapper_last_step(im.bg_uyB.get()),
                        };
                        for (int s : steps)
                            if (s > kBgMaxStepPx || s < -kBgMaxStepPx) {
                                im.bg_scene_cut = true;
                                std::fprintf(stderr,
                                    "[fondos] corte de escena detectado (delta "
                                    "%d px) — stitch congelado; reiniciá la "
                                    "captura para la escena nueva\n", s);
                                break;
                            }
                        // ABI 1.10 (guía §5.1): `h40` describe el frame EMITIDO
                        // (== viewport_w), no reg 12. En el frame de transición
                        // los registros ya cambiaron la geometría y el core la
                        // aplica al siguiente: `GEOMETRY_PENDING` dice que el
                        // viewport y VDP_REGS hablan de dos frames distintos, y
                        // ese frame no se cose — mezclar las dos geometrías es
                        // exactamente lo que el flag vino a impedir.
                        const bool geo_pending = im.sys_ok &&
                            (im.sys.flags & AYTHER_SYSTEM_GEOMETRY_PENDING) != 0;
                        if (!im.bg_scene_cut && !geo_pending) {
                        // h40 lo dice SYSTEM cuando el core lo da (ABI 1.5):
                        // la decodificación de reg 12 ya se corrigió una vez
                        // del lado del core sin que esta copia se enterara.
                        const bool h40 = im.sys_ok ? im.sys.h40 != 0
                                                   : (regs[0x0C] & 0x81) == 0x81;
                        const int scr_cols = h40 ? 40 : 32, scr_rows = 28;
                        struct L { uint8_t plane; uint32_t base; int64_t camx, camy; };
                        const L layers[2] = { { 0, baseA, absxA, absyA },
                                              { 1, baseB, absxB, absyB } };
                        for (const L& l : layers) {
                            const int cam_col = (int)(l.camx / 8), cam_row = (int)(l.camy / 8);
                            // Saltar las últimas columnas MIENTRAS LA CÁMARA SE MUEVE: el
                            // juego streamea la columna entrante 1–2 celdas antes de que
                            // esté en pantalla — leer el borde en movimiento toma tiles
                            // "futuros". : con la cámara QUIETA no hay streaming y el
                            // borde se lee entero — sin esto, el extremo FINAL del paneo
                            // perdía sus últimas columnas para siempre (el ojo de la
                            // tortuga de GA, reporte 2026-07-30).
                            if (l.plane < 2) {
                                const bool cam_moved =
                                    !im.bg_prev_ok[l.plane] ||
                                    im.bg_prev_col[l.plane] != cam_col ||
                                    im.bg_prev_row[l.plane] != cam_row;
                                im.bg_static_frames[l.plane] =
                                    cam_moved ? 0 : im.bg_static_frames[l.plane] + 1;
                            }
                            const int skip_cols =
                                (l.plane < 2 && im.bg_static_frames[l.plane] >= 1) ? 0 : 3;
                            const int gcols = scr_cols - skip_cols, grows = scr_rows;
                            // 1) Leer la GRILLA visible (codes; 0 = celda vacía).
                            std::vector<uint16_t> grid((size_t)gcols * grows, 0);
                            for (int scy = 0; scy < grows; ++scy)
                                for (int scx = 0; scx < gcols; ++scx) {
                                    const int ntc = (((cam_col + scx) % wc) + wc) % wc;
                                    const int ntr = (((cam_row + scy) % hc) + hc) % hc;
                                    const uint16_t wd =
                                        (uint16_t)rd16(l.base + (uint32_t)(ntr * wc + ntc) * 2u);
                                    if ((wd & 0x7FF) == 0) continue;
                                    grid[(size_t)scy * gcols + scx] = wd & 0x7FFF;
                                }
                            // 2) : desplazamiento de CONTENIDO. Comparar contra la
                            // grilla del frame anterior probando corrimientos ±2 celdas
                            // alrededor del que IMPLICAN los registros; si otro matchea
                            // con mayoría clara Y decididamente mejor, el delta extra es
                            // el nivel moviéndose por reescritura de la nametable (GA
                            // vertical) y se acumula como cámara de contenido. En juegos
                            // de scroll por registro el mejor corrimiento es el esperado
                            // → extra 0 → byte-idéntico a lo validado (Sonic/EM-1).
                            int extra_x = 0, extra_y = 0;
                            if (l.plane < 2 && im.bg_prev_ok[l.plane] &&
                                im.bg_prev_grid[l.plane].size() == grid.size()) {
                                const auto& pg = im.bg_prev_grid[l.plane];
                                const int e_gx = cam_col - im.bg_prev_col[l.plane];
                                const int e_gy = cam_row - im.bg_prev_row[l.plane];
                                int best_gx = e_gx, best_gy = e_gy;
                                int best_m = -1, best_n = 0, exp_m = 0;
                                for (int gy = e_gy - 2; gy <= e_gy + 2; ++gy)
                                    for (int gx = e_gx - 2; gx <= e_gx + 2; ++gx) {
                                        int m = 0, n = 0;
                                        for (int sy = 0; sy < grows; ++sy) {
                                            const int py = sy + gy;
                                            if (py < 0 || py >= grows) continue;
                                            for (int sx = 0; sx < gcols; ++sx) {
                                                const int px = sx + gx;
                                                if (px < 0 || px >= gcols) continue;
                                                const uint16_t a = grid[(size_t)sy * gcols + sx];
                                                const uint16_t b = pg[(size_t)py * gcols + px];
                                                if (!a || !b) continue;
                                                ++n; if (a == b) ++m;
                                            }
                                        }
                                        if (gx == e_gx && gy == e_gy) exp_m = m;
                                        if (m > best_m) { best_m = m; best_n = n;
                                                          best_gx = gx; best_gy = gy; }
                                    }
                                // Decisivo: muestra suficiente, ≥80% de match y una
                                // mejora real (no un empate de texturas repetitivas).
                                if ((best_gx != e_gx || best_gy != e_gy) &&
                                    best_n >= 200 && best_m * 10 >= best_n * 8 &&
                                    best_m >= exp_m + best_n / 5) {
                                    extra_x = best_gx - e_gx;
                                    extra_y = best_gy - e_gy;
                                }
                            }
                            im.bg_content_col[l.plane] += extra_x;
                            im.bg_content_row[l.plane] += extra_y;
                            if (l.plane < 2) {
                                im.bg_prev_grid[l.plane] = grid;
                                im.bg_prev_col[l.plane]  = cam_col;
                                im.bg_prev_row[l.plane]  = cam_row;
                                im.bg_prev_ok[l.plane]   = true;
                            }
                            // 3) Observar con la coordenada de NIVEL = registros +
                            // cámara de contenido (la lectura de nametable ya fue por
                            // registros, que es lo físico).
                            //  EM-8.0: la columna de nivel POR BANDA.
                            //
                            // El plano B lleva una entrada de Hscroll por banda,
                            // asi que la columna depende de la FILA. Con una
                            // camara unica las bandas se apilan: medido en
                            // Sonic 2, el plano A reconstruia 607 columnas de
                            // nivel y el B solo 37 —menos de una pantalla— con
                            // 45 bandas por frame. El arte no faltaba, estaba
                            // amontonado en el lugar equivocado.
                            //
                            // La regla vive en `parallax_bands.h` y no aca: la
                            // primera version quedo enterrada en este bucle y el
                            // oraculo del stitcher no la veia —llama al stitcher
                            // directamente—, asi que al medirla no movio un solo
                            // numero. No estaba mal: no se estaba ejecutando.
                            if (l.plane < 2) {
                                im.bg_bands[l.plane].configure(grows, wpx);
                                im.bg_bands[l.plane].observe(rd32, hscb, hmask,
                                                             l.plane);
                            }
                            const int lv_col = cam_col + im.bg_content_col[l.plane];
                            const int lv_row = cam_row + im.bg_content_row[l.plane];
                            for (int scy = 0; scy < grows; ++scy)
                                for (int scx = 0; scx < gcols; ++scx) {
                                    const uint16_t code = grid[(size_t)scy * gcols + scx];
                                    if (!code) continue;
                                    ayther_bg_stitcher_observe(
                                        im.bg_st.get(), l.plane,
                                        lv_col + scx +
                                            (l.plane < 2
                                                 ? im.bg_bands[l.plane].offset_from_top(scy)
                                                 : 0),
                                        lv_row + scy, code);
                                }
                            // Guardar la cámara de ESTE plano (ya con el offset de
                            // contenido: bg_hash y la def de la Panorámica tienen que
                            // vivir en el MISMO espacio de nivel que el stitcher) para
                            // acumular más abajo los hashes de contenido — las celdas
                            // visibles con su hash las llena collect(), bastante después.
                            if (l.plane < 3) {
                                im.bg_camx[l.plane] =
                                    (int32_t)(l.camx + (int64_t)im.bg_content_col[l.plane] * 8);
                                im.bg_camy[l.plane] =
                                    (int32_t)(l.camy + (int64_t)im.bg_content_row[l.plane] * 8);
                                im.bg_cam_ok[l.plane] = true;
                            }
                        }
                        }   // fin !bg_scene_cut ()
                    }
                }
                const int winW = 64, winH = 32;
                const int vbound = regs[0x12] & 0x1F, vdown = (regs[0x12] >> 7) & 1;
                const int hbound = (int)(regs[0x11] & 0x1F) * 2, hright = (regs[0x11] >> 7) & 1;
                auto win_active = [&](int cx, int cy) {
                    const bool full_row = (vdown == (cy >= vbound));   // fila entera = window
                    return full_row || (hright ? (cx >= hbound) : (cx < hbound));
                };
                uint32_t n = 0;
                uint32_t npick = 0;   // celdas visibles registradas (pick-list, asignadas o no)
                uint32_t nborder = 0; // R-3: celdas parciales de borde (array lateral)
                // Fase C: la emisión de overlays va en DOS tiempos — primero un
                // pase único de COLECCIÓN (todas las celdas visibles, ambas
                // prioridades, al pick-list con flips+prio por celda), después
                // el matcher de SETS decide qué celdas consume, y recién ahí se
                // emiten los 1×1 (celdas no consumidas con asset) y los quads
                // de set en su lane (lo bajo los sprites / hi sobre ellos).
                auto collect = [&]() {
                    for (int plane = 0; plane < 3; ++plane) {
                        const bool     is_win = (plane == 2);
                        const uint32_t pbase  = is_win ? baseW : base[plane];
                        const int      pwc    = is_win ? winW  : wc;
                        const int      phc    = is_win ? winH  : hc;
                        for (int cy = 0; cy < phc; ++cy) {
                            for (int cx = 0; cx < pwc; ++cx) {
                                const uint16_t w = (uint16_t)rd16(pbase + (uint32_t)(cy * pwc + cx) * 2u);
                                const uint16_t pattern = w & 0x7FF;
                                if (!pattern) continue;
                                const int want_prio = (int)((w >> 15) & 1);   // prioridad VDP de ESTA celda
                                const uint8_t pal = (w >> 13) & 3;
                                auto it = key2hash.find(((uint32_t)plane << 13) | ((uint32_t)pattern << 2) | pal);
                                if (it == key2hash.end()) continue;
                                int sx, sy;
                                if (is_win) {
                                    // Window: posición directa, sólo dentro del clip.
                                    sx = cx * 8; sy = cy * 8;
                                    if (sx >= sw || sy >= sh || !win_active(cx, cy)) continue;
                                } else {
                                    // Inverso del mapeo línea→celda del VDP. H mueve a la DERECHA
                                    // (screen_x = cx*8 + H), V hacia ARRIBA (screen_y = cy*8 - V).
                                    // Con per-line hscroll un tile abarca 8 líneas con distinto H
                                    // (cizalla). Un quad rígido no cizalla, así que se muestrea H en
                                    // la línea CENTRAL del tile (cy*8+4) → el error se reparte
                                    // ±medio gradiente en vez de acumularse hacia abajo. (En la
                                    // práctica los backgrounds de parallax usan bandas ≥8px → H
                                    // constante dentro del tile → sin cizalla; la cizalla sub-tile
                                    // es de efectos de distorsión que no se sustituyen.)
                                    const int  sline = ((cy * 8 + 4 - V0[plane]) % hpx + hpx) % hpx;
                                    const uint32_t hw = rd32(hscb + (((uint32_t)sline & hmask) << 2));
                                    const int  Hh = (plane == 0) ? (int)(hw & 0x3FF) : (int)((hw >> 16) & 0x3FF);
                                    sx = ((cx * 8 + Hh) % wpx + wpx) % wpx;
                                    // R-3 (): aceptar también la celda PARCIAL del
                                    // borde (screen x/y negativos cuando el scroll no
                                    // está alineado a 8) — sin ella el inventario deja
                                    // una franja de backdrop en el borde. Va al array
                                    // LATERAL, no al pick-list: firma/matchers intactos.
                                    int fx = sx;
                                    if (sx >= sw) {
                                        if (sx <= wpx - 8) continue;
                                        fx = sx - wpx;              // -7..-1 (parcial izq.)
                                    }
                                    int V = V0[plane];                  // V por columna (2-cell) o global
                                    if (two_cell) {
                                        int col2 = fx > 0 ? fx >> 4 : 0;
                                        if (col2 > 19) col2 = 19;
                                        const uint32_t vc = rdvs32(col2);
                                        V = (plane == 0) ? (int)(vc & 0x3FF) : (int)((vc >> 16) & 0x3FF);
                                    }
                                    sy = ((cy * 8 - V) % hpx + hpx) % hpx;
                                    int fy = sy;
                                    if (sy >= sh) {
                                        if (sy <= hpx - 8) continue;
                                        fy = sy - hpx;              // -7..-1 (parcial arriba)
                                    }
                                    if (fx < 0 || fy < 0) {
                                        if (nborder < 512) {
                                            PlaneCellHit& pc = im.plane_cells_border[nborder++];
                                            pc.hash = it->second;
                                            pc.screen_x = (int16_t)fx; pc.screen_y = (int16_t)fy;
                                            pc.plane = (uint8_t)plane;
                                            pc.flags = (uint8_t)(((w >> 11) & 1)
                                                     | (((w >> 12) & 1) << 1)
                                                     | ((uint8_t)want_prio << 2) | 0x08);
                                            pc.pattern = pattern;
                                            pc.palette = pal;
                                        }
                                        continue;
                                    }
                                }
                                // Máscara de oclusión (pase hi): píxel OPACO de tile de
                                // ALTA prioridad → el VDP lo dibuja SOBRE los sprites de
                                // prioridad baja. El compose la usa para que el primer
                                // plano (wipe de transición, arcos) tape el HD de pose
                                // igual que tapa al original.
                                if (want_prio == 1) {
                                    if (im.plane_hi_w != (uint16_t)sw || im.plane_hi_h != (uint16_t)sh) {
                                        im.plane_hi_w = (uint16_t)sw; im.plane_hi_h = (uint16_t)sh;
                                        im.plane_hi_opaque.assign((size_t)sw * sh, 0);
                                    }
                                    const bool phf = ((w >> 11) & 1) != 0, pvf = ((w >> 12) & 1) != 0;
                                    for (int row = 0; row < 8; ++row) {
                                        const int py = sy + row;
                                        if (py >= sh) break;
                                        const int srow = pvf ? 7 - row : row;
                                        for (int col = 0; col < 8; ++col) {
                                            const int px = sx + col;
                                            if (px >= sw) break;
                                            const int scol = phf ? 7 - col : col;
                                            const uint32_t poff = ((uint32_t)pattern * 32u
                                                + (uint32_t)srow * 4u + (uint32_t)(scol >> 1)) ^ 1u;   // vista de bus (word-swap)
                                            const uint8_t pb = poff < vsz ? vram[poff] : 0;
                                            if ((scol & 1) ? (pb & 0x0F) : (pb >> 4))
                                                im.plane_hi_opaque[(size_t)py * sw + px] = 1;
                                        }
                                    }
                                }
                                // Pick-list (sync viewport↔Capas): registrar TODA celda
                                // visible — asignada o no — con posición + flags
                                // (flips y prio POR CELDA, Fase C).
                                if (npick < kMaxPlaneCells) {
                                    PlaneCellHit& pc = im.plane_cells[npick++];
                                    pc.hash = it->second;
                                    pc.screen_x = (int16_t)sx; pc.screen_y = (int16_t)sy;
                                    pc.plane = (uint8_t)plane;
                                    pc.flags = (uint8_t)(((w >> 11) & 1)
                                             | (((w >> 12) & 1) << 1)
                                             | ((uint8_t)want_prio << 2));
                                    pc.pattern = pattern;      // R-3: la fuente de la celda
                                    pc.palette = pal;
                                }
                            }
                        }
                    }
                };
                collect();

                // ── Firma de PANTALLA (Cuadro · CU001) ───────────────────────
                // Se computa ACÁ, después de collect() y ANTES del matcher de
                // sets: la firma describe la pantalla ORIGINAL. Si los sets
                // consumieran celdas primero, autorar una Utilería cambiaría la
                // firma del Cuadro que la contiene.
                //
                // Término por celda = mix(plano, col, fila, hash) con
                // col = screen_x >> 3, que es lo que la hace INVARIANTE A LA
                // FASE sub-celda: todas las celdas de un plano caen en 8k+p con
                // el mismo p, así que un título que tiembla 3 px no cambia la
                // firma. Si p desborda una celda entera, la pantalla scrolleó 8
                // px — o sea que es una Panorámica, no un Cuadro.
                //
                // La combinación es CONMUTATIVA (suma), así que no depende del
                // orden del pick-list y —lo importante— basta acumular por
                // PLANO: la firma de cualquier máscara de capas es la suma de
                // sus planos, sin recorrer las celdas de nuevo.
                //
                // Nota: el pick-list sólo trae celdas cuyo (plano,patrón,paleta)
                // está en el catálogo deduplicado del frame (tope
                // kMaxPlaneTileOccs); es la misma regla en todos los frames, así
                // que la firma es consistente consigo misma.
                for (int p = 0; p < 3; ++p) { scr_sig[p] = 0; scr_cells[p] = 0; }
                for (uint32_t i = 0; i < npick; ++i) {
                    const PlaneCellHit& pc = im.plane_cells[i];
                    if (pc.plane > 2) continue;
                    uint64_t t = pc.hash;
                    t ^= (uint64_t)(uint16_t)(pc.screen_x >> 3) * 0x9E3779B97F4A7C15ull;
                    t ^= (uint64_t)(uint16_t)(pc.screen_y >> 3) * 0xC2B2AE3D27D4EB4Full;
                    t ^= (uint64_t)(pc.plane + 1) * 0x165667B19E3779F9ull;
                    t ^= t >> 33; t *= 0xFF51AFD7ED558CCDull; t ^= t >> 29;
                    scr_sig[pc.plane] += t;      // conmutativo: sin orden, sin sort
                    ++scr_cells[pc.plane];
                }

                // ── Reconocimiento del CUADRO (CU001) ────────────────────────
                // Por COBERTURA, no por igualdad: una sola celda animada
                // tiraría la firma exacta, y en una pantalla de título eso pasa
                // todo el tiempo. Dos guardas, no una — sin `max_extra`, una
                // pantalla que es SUPERCONJUNTO (un menú encima del título)
                // matchearía el título con cobertura perfecta.
                if (im.hd_enabled && !im.screens.empty()) {
                    // Índice del frame: key(plano,col,fila) → hash. Uno solo
                    // para todos los Cuadros declarados.
                    std::unordered_map<uint32_t, uint64_t> now;
                    now.reserve(npick);
                    for (uint32_t i = 0; i < npick; ++i) {
                        const PlaneCellHit& pc = im.plane_cells[i];
                        if (pc.plane > 2) continue;
                        const uint32_t k = ((uint32_t)pc.plane << 24)
                                         | ((uint32_t)(uint8_t)(pc.screen_x >> 3) << 8)
                                         |  (uint32_t)(uint8_t)(pc.screen_y >> 3);
                        now.emplace(k, pc.hash);
                    }
                    // Reconocimiento EXACTO, capa por capa. Toda variación —por
                    // mínima que sea— es otro Cuadro: una llama que titila no es
                    // ruido a filtrar, es contenido, y le corresponde su propio
                    // Cuadro (o factorizarla como Utilería, o taparla con un
                    // video en la Cinemática). Antes esto era cobertura con
                    // umbral (0.92/0.08), que descartaba en silencio lo que
                    // cambiaba y hacía que dos pantallas parecidas colapsaran
                    // en una. `min_match`/`max_extra` quedan en la definición
                    // como legado y ya no deciden nada.
                    //
                    // El asset NO participa: un Cuadro sin asset se RECONOCE
                    // igual. Antes se lo saltaba antes de mirarlo, y eso rompía
                    // el caso de una Cinemática de N Cuadros cubierta por UN
                    // video — ningún paso se reconocía y la secuencia no
                    // avanzaba nunca. El asset gatea DIBUJAR y RECLAMAR, no
                    // reconocer.
                    // ── Gate por PRESENCIA ( mecanismo 2) ────────────
                    // Contenido presente >=75% por capa declarada, en cualquier
                    // posición. Invariante al scroll y a elementos que se
                    // mueven (el isologotipo del título baja 1 px/frame y la
                    // firma exacta no matchea hasta que se posa). Sin
                    // histéresis: el margen del umbral ya la hace innecesaria
                    // y el consumidor (gate de Acetato) tolera un frame suelto.
                    im.screen_presence_n = 0;
                    {
                        std::unordered_set<uint64_t> present[3];
                        for (uint32_t i = 0; i < npick; ++i) {
                            const PlaneCellHit& pc = im.plane_cells[i];
                            if (pc.plane > 2) continue;
                            present[pc.plane].insert(pc.hash);
                        }
                        // 0.6: medido en el tÃ­tulo de Golden Axe el ratio real es 0.78-1.0
                        // (0.78 con el isologotipo a medio entrar) y en las
                        // pantallas ajenas 0.00 — la separaciÃ³n es binaria,
                        // el umbral sÃ³lo tiene que quedar lejos de ambos.
                        constexpr float kPresenceRatio = 0.6f;
                        for (const auto& [id, def] : im.screens) {
                            if (def.cells.empty() || !def.mask) continue;
                            bool ok = true;
                            for (int p = 0; p < 3 && ok; ++p) {
                                if (!(def.mask & (1u << p))) continue;
                                const auto& want = def.hashes_plane[p];
                                if (want.empty()) { ok = false; break; }
                                uint32_t hit = 0;
                                for (uint64_t hsh : want)
                                    if (present[p].count(hsh)) ++hit;
                                ok = (float)hit >=
                                     kPresenceRatio * (float)want.size();
                            }
                            if (ok && im.screen_presence_n < 8)
                                im.screen_presence[im.screen_presence_n++] = id;
                        }
                    }

                    uint64_t best_id = 0; float best_sc = 0.0f, best_ex = 0.0f;
                    for (const auto& [id, def] : im.screens) {
                        if (def.cells.empty() || !def.mask) continue;
                        bool all = true;
                        for (int p = 0; p < 3 && all; ++p) {
                            if (!(def.mask & (1u << p))) continue;   // capa ajena: no incumbe
                            if (scr_sig[p] != def.sig_plane[p] ||
                                scr_cells[p] != def.cells_plane[p]) all = false;
                        }
                        if (!all) continue;
                        // Empate entre dos Cuadros con la misma firma: gana el
                        // id menor, para que la elección sea determinista.
                        if (!best_id || id < best_id) best_id = id;
                    }
                    if (best_id) { best_sc = 1.0f; best_ex = 0.0f; }
                    // Histéresis ASIMÉTRICA: entrar pide 2 frames consecutivos
                    // (un wipe de transición puede acertar por un frame suelto);
                    // salir es inmediato, que es lo que el spec pide para poder
                    // cancelar una Cinemática en el acto.
                    if (best_id && best_id == im.screen_active) {
                        im.screen_cand = best_id; im.screen_streak = 2;
                    } else if (best_id && (best_id == im.screen_cand ||
                                           im.screen_jump)) {
                        im.screen_cand = best_id;
                        // Saltando, la confirmación es inmediata: no hay frame
                        // anterior con el que hacer racha y el usuario está
                        // parado mirando ESTE.
                        if (im.screen_jump) im.screen_streak = 2;
                        else                ++im.screen_streak;
                        if (im.screen_streak >= 2) im.screen_active = best_id;
                    } else if (best_id) {
                        im.screen_cand = best_id; im.screen_streak = 1;
                        if (im.screen_active && im.screen_active != best_id)
                            im.screen_active = 0;          // salida inmediata
                    } else {
                        im.screen_cand = 0; im.screen_streak = 0; im.screen_active = 0;
                    }
                    im.screen_score = best_sc; im.screen_extra = best_ex;
                    im.screen_jump  = false;   // consumido por este produce

                    // ── CINEMÁTICA (CU004): tick de la secuencia ─────────────
                    // Va ACÁ y no más abajo porque es el único punto donde
                    // conviven el Cuadro ya confirmado por la histéresis y el
                    // quad todavía sin escribir — y porque tiene que cobrar
                    // ANTES que el Cuadro (Kinematic > Picture en la escalera).
                    if (!im.kinematics.empty()) {
                        const int64_t fnow = (int64_t)im.frame_index;
                        if (fnow != im.kine_last_frame) {
                            // Un salto NO es «la secuencia se rompió»: es que el
                            // artista scrubbeó. Se re-evalúa desde cero, que con
                            // arranque en cualquier paso cae donde corresponde.
                            if (fnow != im.kine_last_frame + 1) im.kinematic_reset();
                            im.kinematic_tick(im.screen_active);
                            im.kine_last_frame = fnow;
                        }
                    } else {
                        im.kinematic_reset();
                    }

                    im.screen_sub_n = 0;
                    {
                        // Resolución por RANGO: si hay Cinemática en curso y su
                        // paso trae asset propio, ese gana; si no, cae al del
                        // Cuadro. No se emiten los dos — el quad es opaco y a
                        // pantalla completa, así que dibujar ambos dejaría el
                        // resultado a merced del orden de lane, que es
                        // exactamente el bug que la escalera existe para evitar.
                        const std::string* pick = nullptr;
                        // : el Cuadro y la Cinemática son PLANOS (una
                        // pantalla entera de ellos) y viajan en el `planes` del
                        // manifest — así que respetan ese subsistema como ya lo
                        // hacen la Panorámica y los plane sets. El gate va ACÁ,
                        // en la emisión, y NO en el reconocimiento de arriba: el
                        // `screen_match_id`/`presence` alimenta además los gates
                        // de los Acetatos (/), que son contenido
                        // agregado y no dependen de que se reemplace el plano.
                        // Sin esto, el perfil «original» del pack (todos los
                        // subsistemas apagados) seguía sustituyendo la pantalla
                        // completa.
                        const bool screens_on = im.sub_on(Subsystem::Planes);
                        if (screens_on && im.kine_active) {
                            auto kt = im.kinematics.find(im.kine_active);
                            if (kt != im.kinematics.end() &&
                                im.kine_step < kt->second.assets.size() &&
                                !kt->second.assets[im.kine_step].empty())
                                pick = &kt->second.assets[im.kine_step];
                        }
                        if (screens_on && !pick && im.screen_active) {
                            auto it = im.screens.find(im.screen_active);
                            if (it != im.screens.end() && !it->second.asset.empty())
                                pick = &it->second.asset;
                        }
                        // Un `.ivf` NO puede salir por `screen_sub`: su
                        // `asset_path` sería la clave del cache de texturas de
                        // VkSprite, la textura entraría a la liberación
                        // diferida del staging () y a los pocos frames
                        // dejaría de aceptar uploads EN SILENCIO — el video
                        // quedaría congelado en su primer frame sin ningún
                        // error. Va por su propio camino, en píxeles.
                        const bool is_video =
                            pick && pick->size() > 4 &&
                            pick->compare(pick->size() - 4, 4, ".ivf") == 0;

                        if (is_video) {
                            im.video_tick(*pick);
                        } else {
                            im.video_reset();
                            if (pick) {
                                AytherSpriteSub& q = im.screen_sub;
                                std::memset(&q, 0, sizeof(q));
                                std::snprintf(q.asset_path, sizeof(q.asset_path), "%s",
                                              pick->c_str());
                                q.screen_x = 0; q.screen_y = 0;
                                q.w_px = (uint16_t)sw; q.h_px = (uint16_t)sh;
                                q.w_tiles = (uint8_t)(sw / 8 > 255 ? 255 : sw / 8);
                                q.h_tiles = (uint8_t)(sh / 8 > 255 ? 255 : sh / 8);
                                q.palette = 0xFF; q.synth_pal = 0xFF;
                                im.screen_sub_n = 1;
                            }
                        }
                    }
                } else {
                    im.screen_active = 0; im.screen_cand = 0; im.screen_streak = 0;
                    im.screen_sub_n = 0; im.screen_score = 0.0f; im.screen_extra = 0.0f;
                    im.screen_presence_n = 0;
                    // Sin esto la Cinemática no se cancela: se CONGELA con su
                    // último estado cuando se apaga el HD o se borran los Cuadros.
                    im.kinematic_reset();
                    im.video_reset();   // idem el video: sin esto queda el último frame
                }

                // ── Captura de la tira: hashes de CONTENIDO ──────────────────
                // El stitcher (más arriba) guarda el CÓDIGO de nametable, que
                // sirve para re-dibujar la tira pero NO para reconocerla: la
                // Panorámica matchea por el hash de contenido, el mismo que
                // traen las celdas visibles. Se acumula acá porque es donde las
                // celdas ya existen — collect() corre después del stitcher.
                // Entran TODAS las lecturas de una posición (el set es de
                // PARES): cada estado de una celda animada tiene que poder
                // anclar. Decía «primera aparición gana», que no es lo que
                // hace — y esa diferencia es la de : el PNG conserva
                // UNA (`Cell::last`, background.rs) y el índice, todas.
                if (im.bg_capture_on && npick > 0) {
                    for (uint32_t i = 0; i < npick; ++i) {
                        const PlaneCellHit& pc = im.plane_cells[i];
                        if (pc.plane > 2 || !im.bg_cam_ok[pc.plane]) continue;
                        const int32_t lx = (im.bg_camx[pc.plane] + pc.screen_x) >> 3;
                        const int32_t ly = (im.bg_camy[pc.plane] + pc.screen_y) >> 3;
                        const uint64_t bk = Impl::pano_key(lx, ly);
                        im.bg_hash[pc.plane].insert({ bk, pc.hash });
                        // : y cuál DIBUJA. Último gana, igual que
                        // `Cell::last` del stitcher, que es de donde el
                        // exportador saca el código del PNG.
                        im.bg_hash_drawn[pc.plane][bk] = pc.hash;
                    }
                }

                // ── PANORÁMICA: anclaje por CONTENIDO (CU003) ────────────────
                // Cada celda visible cuyo hash está en la tira dice dónde está
                // la cámara: si esa celda vive en la columna de nivel `lx` y se
                // ve en `screen_x`, entonces `cam_px = lx*8 - screen_x`. La
                // MODA de esos votos la fija exacta, en píxeles, sin depender
                // de ningún acumulado — así sobrevive a un seek, a una carga de
                // savestate y a un corte de escena, que es justo lo que
                // `plane_cam_*` no puede.
                im.pano_id = 0; im.pano_votes = 0; im.pano_cells = 0;
                im.pano_valid = false; im.pano_cover = 0;
                if (!im.panoramas.empty() && npick > 0) {
                    for (const auto& [pid, pd] : im.panoramas) {
                        if (pd.anchors.empty()) continue;
                        std::unordered_map<uint64_t, uint32_t> tally;   // (cam_x,cam_y) → votos
                        std::vector<ayther::PanoVote> pvotes;   // 
                        uint32_t voters = 0;
                        for (uint32_t i = 0; i < npick; ++i) {
                            const PlaneCellHit& pc = im.plane_cells[i];
                            if (pc.plane != pd.plane) continue;
                            // : la celda vota aunque el juego la haya
                            // reasignado a otra línea de CRAM.
                            const auto* anch =
                                Impl::pano_find_anchor(pd, pc.hash, pc.palette);
                            if (!anch) continue;
                            ++voters;
                            for (const auto& [lx, ly] : *anch) {
                                const int32_t cx = lx * 8 - pc.screen_x;
                                const int32_t cy = ly * 8 - pc.screen_y;
                                ++tally[((uint64_t)(uint32_t)cx << 32)
                                        | (uint32_t)cy];
                                // : el MISMO voto, guardado con su línea de
                                // pantalla. Con line-scroll la línea es lo que
                                // dice a qué banda pertenece; sin bandas el
                                // dato sobra y no cuesta nada.
                                pvotes.push_back(ayther::PanoVote{
                                    pc.screen_y, cx, cy});
                            }
                        }
                        // Desempate DETERMINISTA: con `>` a secas el ganador de
                        // un empate lo decidía el orden de iteración de un
                        // unordered_map, así que el MISMO frame podía dar
                        // anclajes distintos. Ante empate gana la clave menor.
                        //
                        // Y CONTINUIDAD: un hash raro puede repetirse en otro
                        // tramo del nivel, así que la moda cruda tiene outliers
                        // — frames sueltos donde gana un candidato lejano y la
                        // cámara "salta". Entre candidatos con votos parecidos
                        // (>=80% del mejor) se prefiere el más cercano al último
                        // anclaje válido. El primer frame, o el que sigue a un
                        // salto, no tiene referencia y cae en la moda pura, que
                        // es lo que hace que un seek siga anclando bien.
                        uint64_t bestk = 0; uint32_t bestv = 0;
                        for (const auto& [k, v] : tally)
                            if (v > bestv || (v == bestv && k < bestk)) { bestv = v; bestk = k; }
                        if (bestv && im.pano_last_id == pid) {
                            const uint32_t floor_v = bestv - bestv / 5;   // 80%
                            int64_t best_d = INT64_MAX;
                            uint64_t neark = bestk;
                            for (const auto& [k, v] : tally) {
                                if (v < floor_v) continue;
                                const int32_t kx = (int32_t)(uint32_t)(k >> 32);
                                const int32_t ky = (int32_t)(uint32_t)(k & 0xFFFFFFFFull);
                                const int64_t dx = (int64_t)kx - im.pano_last_x;
                                const int64_t dy = (int64_t)ky - im.pano_last_y;
                                const int64_t d  = dx * dx + dy * dy;
                                if (d < best_d || (d == best_d && k < neark)) {
                                    best_d = d; neark = k;
                                }
                            }
                            bestk = neark;
                            bestv = tally[neark];
                        }
                        // Un solo voto no ancla nada (un tile raro puede
                        // repetirse fuera de la tira); pedir al menos 2 Y una
                        // MAYORÍA CLARA de los votantes: con una mayoría flaca
                        // el anclaje es dudoso y publicarlo como válido hacía
                        // que el renderer dibujara la tira corrida. Sin llegar
                        // al piso, la cámara queda «no anclada» — que es una
                        // respuesta útil, a diferencia de una inventada.
                        const bool strong =
                            voters > 0 &&
                            bestv * 100u >= voters * kPanoramaMinVotePct;
                        // VERIFICACIÓN DE COBERTURA (2026-07-30): la moda puede
                        // ser «fuerte» con UN solo hash repetido — en una
                        // pantalla negra, cientos de tiles idénticos votan las
                        // pocas posiciones donde ese hash vive en la tira y el
                        // ancla salía inventada (la Panorámica aparecía sobre el
                        // fundido, f33–86 de la demo — y con el rect completo
                        // eso dibuja la tira entera). El ancla vale sólo si la
                        // tira EXPLICA la pantalla: una fracción decisiva de las
                        // celdas visibles del plano coincide hash-a-posición.
                        bool explains = false;
                        //  EM-8.1: cuanto de la pantalla EXPLICA la tira.
                        // `explains` es el si/no de siempre; el porcentaje se
                        // guarda porque el area EXTENDIDA necesita mas certeza
                        // que el area nativa — ver la nota en la emision.
                        uint32_t cover_pct = 0;
                        if (bestv >= 2 && strong) {
                            constexpr uint32_t kPanoramaMinCoverPct = 35;
                            const int32_t ccx = (int32_t)(uint32_t)(bestk >> 32);
                            const int32_t ccy = (int32_t)(uint32_t)(bestk & 0xFFFFFFFFull);
                            uint32_t seen = 0, match = 0;
                            for (uint32_t i = 0; i < npick; ++i) {
                                const PlaneCellHit& pc = im.plane_cells[i];
                                if (pc.plane != pd.plane) continue;
                                ++seen;
                                auto hit2 = pd.by_pos.find(Impl::pano_key(
                                    (ccx + pc.screen_x) >> 3,
                                    (ccy + pc.screen_y) >> 3));
                                if (hit2 == pd.by_pos.end()) continue;
                                if (Impl::pano_pos_matches(hit2->second, pc.hash,
                                                           pc.palette)) ++match;
                            }
                            explains = match >= 24 &&
                                       match * 100u >= seen * kPanoramaMinCoverPct;
                            cover_pct = seen ? (uint32_t)(match * 100u / seen) : 0u;
                        }
                        if (bestv >= 2 && strong && explains && bestv > im.pano_votes) {
                            im.pano_id    = pid;
                            im.pano_votes = bestv;
                            im.pano_cells = voters;
                            im.pano_cam_x = (int32_t)(uint32_t)(bestk >> 32);
                            im.pano_cam_y = (int32_t)(uint32_t)(bestk & 0xFFFFFFFFull);
                            im.pano_valid = true;
                            im.pano_cover = cover_pct;
                            im.pano_last_id = pid;
                            im.pano_last_x  = im.pano_cam_x;
                            im.pano_last_y  = im.pano_cam_y;
                            // : y la cámara POR BANDA. Los cortes salen
                            // de la tabla Hscroll del plano de esta tira; con
                            // reg $0B en modo 0 (scroll entero) sale UNA banda
                            // y la emisión es idéntica a la de siempre — el
                            // 100 % del corpus medido salvo Sonic 3 & K.
                            {
                                AYTHER_LEGACY_READ_BEGIN
                                const uint8_t* rg = im.regs_ptr();
                                const uint8_t* vm = im.vram_ptr();
                                const size_t   vs = im.runner.video_ram_size();
                                AYTHER_LEGACY_READ_END
                                const uint8_t r0b = rg ? rg[0x0B] : 0;
                                const uint8_t r0d = rg ? rg[0x0D] : 0;
                                // La tabla lleva los dos planos por entrada: A
                                // en los 16 bits bajos, B en los altos. Es el
                                // mismo armado que hscroll_bands_probe.
                                auto rd32 = [vm, vs](uint32_t a) -> uint32_t {
                                    if (!vm || a + 3 >= vs) return 0;
                                    const uint32_t lo = (uint32_t)vm[a]
                                                      | ((uint32_t)vm[a + 1] << 8);
                                    const uint32_t hi = (uint32_t)vm[a + 2]
                                                      | ((uint32_t)vm[a + 3] << 8);
                                    return lo | (hi << 16);
                                };
                                const int rows = (int)(im.snap.h ? im.snap.h / 8 : 28);
                                const auto edges = ayther::pano_band_edges(
                                    rd32, ayther::hscroll_base(r0d),
                                    ayther::hscroll_mask(r0b), pd.plane, rows);
                                im.pano_bandcams = ayther::pano_vote_by_band(
                                    pvotes.data(), pvotes.size(),
                                    edges.data(), edges.size(),
                                    (int32_t)(rows * 8));
                            }
                        }
                    }
                }

                // ── COBERTURA RECLAMADA (escalera de ayther_rank.h) ──────────
                // Marca por celda del pick-list qué se llevó una entidad de
                // MAYOR complejidad. Los matchers corren de mayor a menor rango
                // y el que gana RECLAMA su cobertura, así que los de abajo ya
                // no la ven. Se declara acá arriba porque el primero en cobrar
                // es el Cuadro y el siguiente la Panorámica.
                //
                // DIMENSIONADO YA: cada matcher tiene que poder reclamar sin
                // depender de que otro haya corrido antes. Estaba vacío hasta
                // que un CUADRO lo llenaba, y el reclamo de la Panorámica —que
                // exige `consumed.size() == npick`— no corría nunca en las
                // pantallas sin Cuadro, que son justamente las de scroll donde
                // vive una Panorámica: las celdas quedaban sin reclamar y el
                // plano original se seguía emitiendo debajo (y ENCIMA en las de
                // prioridad alta, que van en la lane de frente).
                std::vector<uint8_t> consumed(npick, 0);

                // ── Rango CUADRO: reclama la pantalla de sus capas ───────────
                // Un Cuadro es la pantalla entera en HD, así que la Panorámica,
                // la Utilería y los tiles 1×1 que caen DENTRO suyo ya están
                // dibujados en ese asset: emitirlos otra vez los duplicaría
                // encima. Antes esto no se hacía y el renderer los pintaba
                // sobre el Cuadro — con un comentario que decía que era a
                // propósito.
                // El Cuadro que reclama puede ser el CONFIRMADO o, si la
                // Cinemática se sostiene en un frame sin confirmación (el hueco
                // de un frame que deja toda transición limpia), el paso que la
                // Cinemática está esperando. Sin esto, en ese frame la
                // Panorámica, la Utilería y los tiles 1×1 se dibujan ENCIMA del
                // quad full-screen — un parpadeo de un frame por cada corte.
                // SÓLO reclama si hay algo que DIBUJAR. Un Cuadro reconocido
                // pero sin asset asignado no tapa nada: el artista todavía no
                // decidió reemplazar esa pantalla, así que la Utilería y los
                // tiles 1×1 que sí tengan asset se siguen dibujando sobre el
                // original. Es lo que habilita el flujo de la estrella que
                // titila — se autora sólo la estrella como Utilería y el resto
                // de la pantalla queda como está.
                //
                // En cuanto el Cuadro RECIBE un asset, se lleva la capa entera
                // y la Utilería deja de contar: ya está pintada en esa imagen.
                // `screen_sub_n` es exactamente esa condición — se emitió más
                // arriba, y vale igual si el asset lo puso la Cinemática.
                // `vid_on` cuenta igual que `screen_sub_n` (): un paso-video
                // NO emite quad, pero cubre la pantalla entera exactamente
                // igual. Sin incluirlo acá, el reclamo no ocurre y la
                // Panorámica, las Utilerías y los tiles de plano se dibujan
                // ENCIMA del video — el mismo bug de cobertura que el diseño ya
                // vivió dos veces.
                const bool covers = im.screen_sub_n || im.vid_on;
                uint64_t claim_id = covers ? im.screen_active : 0;
                if (!claim_id && covers && im.kine_active) {
                    auto kt = im.kinematics.find(im.kine_active);
                    if (kt != im.kinematics.end() && im.kine_step < kt->second.steps.size())
                        claim_id = kt->second.steps[im.kine_step];
                }
                if (claim_id && npick > 0) {
                    auto it = im.screens.find(claim_id);
                    if (it != im.screens.end()) {
                        // Reclama la CAPA ENTERA: un Cuadro es sus capas
                        // completas, no un subconjunto de celdas. Su asset es
                        // una imagen opaca de toda la pantalla, así que TODO lo
                        // de esas capas ya está pintado ahí — incluida una
                        // Utilería que caiga adentro.
                        for (uint32_t i = 0; i < npick; ++i) {
                            const PlaneCellHit& pc = im.plane_cells[i];
                            if (pc.plane > 2 || !(it->second.mask & (1u << pc.plane)))
                                continue;
                            consumed[i] = 1;
                        }
                    }
                }

                // ── PANORÁMICA: los quads de la tira (Fase 3b) ───────────────
                // POR QUÉ NO ES UN SOLO QUAD A PANTALLA COMPLETA. Ese era el
                // bloqueo de esta fase: una tira opaca del tamaño de la
                // pantalla tapa TODO lo que hay debajo, incluidos los píxeles
                // originales del otro plano y cualquier primer plano de su
                // propia capa — un plano A de primer plano sobre una panorámica
                // de plano B se borraba. La salida que se había anotado era una
                // máscara por píxel, con su textura y su shader.
                //
                // No hace falta: se emite UN QUAD POR TRAMO de celdas contiguas
                // que SON la tira, cada uno con su sub-rect UV. El recorte por
                // celda ES la máscara, con la granularidad exacta en la que la
                // Panorámica está definida, sin pase nuevo ni textura extra.
                // Y como cada quad es opaco y cae justo sobre las celdas que
                // reemplaza, tampoco hace falta suprimir el original — el mismo
                // argumento del Cuadro, y se evita la latencia de 1 frame del
                // canal de supresión y el problema de los hashes compartidos con
                // una Utilería.
                //
                // La pertenencia se decide por CONTENIDO, no por posición: se
                // exige que el hash observado sea el que la tira tiene en esa
                // posición de nivel. Un HUD o un primer plano dibujado sobre el
                // mismo plano no coincide y queda intacto.
                im.pano_subs.clear();
                if (im.pano_valid && im.hd_enabled
                    && im.sub_on(Subsystem::Planes)) {   // : la tira es un plano
                    auto it = im.panoramas.find(im.pano_id);
                    if (it != im.panoramas.end() && !it->second.asset.empty()) {
                        const Impl::PanoramaDef& pd = it->second;
                        //  parte 2: UN quad con el RECT COMPLETO de la tira,
                        // anclado AL PÍXEL a la cámara votada. El quad-por-tramo
                        // de celdas verificadas era necesario cuando la lane era
                        // global (no podía tapar A/sprites); desde el z-inline la
                        // tira dibuja en el pase de SU plano y lo de adelante le
                        // queda encima — puede cubrir el rect entero, que es lo
                        // que «reemplazar la capa» significa. Con el per-celda,
                        // las no matcheadas (nubes que derivan, bordes con fine
                        // scroll) dejaban ver el plano original detrás y el
                        // redondeo por celda corría el borde superior (reporte
                        // 2026-07-30). Los huecos del PNG (alpha 0) dejan ver el
                        // original: la cobertura la decide el ARTE.
                        // : UN quad POR BANDA. Con line-scroll cada banda
                        // tiene su propia cámara y una tira rígida no las
                        // explica (medido: Sonic 3 & K, 37 bandas en el plano
                        // B). Sin bandas —el 100 % del corpus salvo ese juego—
                        // el loop da UNA vuelta con la cámara de siempre y el
                        // quad sale idéntico byte a byte.
                        const int32_t rw = (int32_t)pd.w_cells * 8;
                        const int32_t rh = (int32_t)pd.h_cells * 8;
                        //  fase 0: con ensanchado, la tira se recorta al
                        // ancho LÓGICO y no al de pantalla — y eso es lo que
                        // llena el área extendida. El arte de los lados sale de
                        // la LÁMINA (lo que cada posición mostró cuando estuvo
                        // en pantalla), no de la nametable: leerla de más
                        // devuelve arte de otro tramo del nivel, porque envuelve
                        // cada 512 px. Sin ensanchar, idéntico a antes.
                        //  fase 0: con ensanchado, la tira se recorta al
                        // ancho LÓGICO y no al de pantalla — y eso es lo que
                        // llena el área extendida. El arte de los lados sale de
                        // la LÁMINA (lo que cada posición mostró cuando estuvo
                        // en pantalla), no de la nametable: leerla de más
                        // devuelve arte de otro tramo del nivel, porque envuelve
                        // cada 512 px. Sin ensanchar, idéntico a antes.
                        //
                        //  EM-8.1, PENDIENTE Y MEDIDO: lo que la tira ponga
                        // en el área extendida es lo que se ve — ahí no hay
                        // celdas vivas que lo corrijan, a diferencia del área
                        // nativa, donde lo que la tira no reclamó se dibuja
                        // encima y tapa un anclaje flojo. Con una tira AMBIGUA
                        // (`clean_pct` bajo: varias hashes por posición, por
                        // celda animada o por un barrido que cruzó de zona) el
                        // anclaje se corrobora contra un hash que el PNG no
                        // dibuja, y los laterales traen otro nivel. Medido en
                        // Sonic 3 & K f2092: cobertura 100 %, lámina de Angel
                        // Island sobre una cueva, frame nativo perfecto.
                        //
                        // NO se gatea acá por cobertura ni por `clean_pct`: los
                        // dos números se probaron como umbral y no separan los
                        // casos —Golden Axe extiende BIEN con cobertura 69 % y
                        // Sonic extiende MAL con 100 %—, así que cualquier piso
                        // rompe un caso bueno para tapar uno malo. El arreglo
                        // real es alinear el índice de anclaje con lo que la
                        // lámina dibuja (un hash por posición), no un umbral.
                        const int32_t sw_px = (int32_t)(im.wide_w_eff > im.snap.w
                                                        ? im.wide_w_eff : im.snap.w);
                        const int32_t sh_px = (int32_t)im.snap.h;
                        struct Slice { int32_t y0, y1, cam_x, cam_y; };
                        std::vector<Slice> slices;
                        if (im.pano_bandcams.size() > 1) {
                            for (const auto& b : im.pano_bandcams) {
                                // Una banda SIN votos no tiene cámara propia:
                                // cae a la global en vez de dibujarse en el
                                // origen, que sería un salto visible.
                                slices.push_back(Slice{
                                    b.y0, b.y1,
                                    b.decided() ? b.cam_x : im.pano_cam_x,
                                    b.decided() ? b.cam_y : im.pano_cam_y});
                            }
                        } else {
                            slices.push_back(Slice{0, sh_px, im.pano_cam_x,
                                                   im.pano_cam_y});
                        }
                        for (const Slice& sl : slices) {
                            //  fase 0: la tira vive en el MISMO espacio
                            // lógico que la escena centrada, así que lleva el
                            // mismo desplazamiento. Sin ensanchar es 0 y el
                            // rect sale en el píxel de siempre.
                            const int32_t wide_dx =
                                (int32_t)(im.wide_w_eff > im.snap.w
                                          ? (im.wide_w_eff - im.snap.w) / 2 : 0);
                            const int32_t rx = pd.origin_x * 8 - sl.cam_x + wide_dx;
                            const int32_t ry = pd.origin_y * 8 - sl.cam_y;
                            const int32_t cx0 = rx > 0 ? rx : 0;
                            int32_t cy0 = ry > 0 ? ry : 0;
                            const int32_t cx1 = (rx + rw < sw_px) ? rx + rw : sw_px;
                            int32_t cy1 = (ry + rh < sh_px) ? ry + rh : sh_px;
                            // Recorte a la banda: fuera de su rango de líneas,
                            // esta cámara no manda.
                            if (cy0 < sl.y0) cy0 = sl.y0;
                            if (cy1 > sl.y1) cy1 = sl.y1;
                            if (cx1 > cx0 && cy1 > cy0 && rw > 0 && rh > 0) {
                                AytherSpriteSub q{};
                                std::snprintf(q.asset_path, sizeof(q.asset_path), "%s",
                                              pd.asset.c_str());
                                q.screen_x = (int16_t)cx0;
                                q.screen_y = (int16_t)cy0;
                                q.w_px = (uint16_t)(cx1 - cx0);
                                q.h_px = (uint16_t)(cy1 - cy0);
                                q.w_tiles = (uint8_t)std::min<int32_t>((cx1 - cx0) / 8, 255);
                                q.h_tiles = (uint8_t)std::min<int32_t>((cy1 - cy0) / 8, 255);
                                q.u0 = (float)(cx0 - rx) / (float)rw;
                                q.v0 = (float)(cy0 - ry) / (float)rh;
                                q.uw = (float)(cx1 - cx0) / (float)rw;
                                q.vh = (float)(cy1 - cy0) / (float)rh;
                                q.palette = 0xFF; q.synth_pal = 0xFF;
                                im.pano_subs.push_back(q);
                            }
                        }
                        // Reclamar la cobertura VERIFICADA por contenido: las
                        // celdas del plano que SON de la tira no se re-emiten en
                        // las lanes 1×1 (MatchRank::Panorama > Tile). Se exige el
                        // hash en su posición — un HUD o primer plano del mismo
                        // plano no coincide y conserva sus canales propios.
                        for (uint32_t i = 0; i < npick; ++i) {
                            const PlaneCellHit& pc = im.plane_cells[i];
                            if (pc.plane != pd.plane || consumed[i]) continue;
                            const int32_t lx = (im.pano_cam_x + pc.screen_x) >> 3;
                            const int32_t ly = (im.pano_cam_y + pc.screen_y) >> 3;
                            auto hit = pd.by_pos.find(Impl::pano_key(lx, ly));
                            if (hit == pd.by_pos.end()) continue;
                            if (Impl::pano_pos_matches(hit->second, pc.hash,
                                                       pc.palette)) consumed[i] = 1;
                        }
                    }
                }

                // ── Matcher de plane SETS (Pintar Fase C · P3 2026-07-25): por
                // aparición del elemento verificar los miembros en sus offsets
                // exactos → un quad del asset estirado al bbox en la lane de la
                // prio del ancla, celdas consumidas fuera de los 1×1 y tiles
                // suprimidos por identidad (mismo canal/latencia que el ojo).
                //
                // P3 TOLERANCIA OFF-SCREEN: un miembro cuya posición esperada
                // cae FUERA del área visible (mismo criterio de bordes que
                // collect(): x∈[0,sw) · y∈[0,sh) — los tiles parcialmente
                // visibles del borde derecho/inferior SÍ están en el pick-list
                // y siguen siendo exigidos) se EXCUSA: el quad se emite igual,
                // parcialmente fuera de pantalla, y el elemento no «popea» al
                // entrar/salir con el scroll.
                //
                // P3 ANCLA-MENOS-FRECUENTE: las anclas se prueban por miembro
                // en orden de FRECUENCIA del hash en este frame (la menos común
                // primero → casi O(apariciones reales) aunque el top-left sea
                // un tile genérico); una aparición cuyo miembro raro quedó
                // fuera de pantalla igual se encuentra por las anclas
                // siguientes. Dedup por ORIGEN del bbox (no se emite dos veces
                // la misma aparición desde anclas distintas).
                // COBERTURA compartida de las celdas visibles: qué celda ya se
                // llevó una entidad de MAYOR complejidad. Es la pieza que hace
                // cumplir la escalera (ayther_rank.h): los matchers corren de
                // mayor a menor rango y el que gana RECLAMA su cobertura, así
                // que los de abajo ya no la ven.
                std::vector<AytherSpriteSub> setq[2];   // [0]=lo · [1]=hi
                std::vector<uint8_t>         setq_plane[2];   // paralelo: plano del set
                // El matcher SUPRIME los tiles originales, así que en modo
                // Original dejaría agujeros: con el HD apagado no corre. El
                // flag se lee POR PRODUCE (produce_frame también corre en el
                // re-render bare del compose y en export_frame).
                if (im.hd_enabled && im.sub_on(Subsystem::Planes)   // 
                    && !im.plane_sets.empty() && npick > 0) {
                    // (`consumed` ya viene dimensionado con lo que reclamaron
                    //  las entidades de mayor rango — no se pisa.)
                    auto poskey = [](uint8_t plane, int px, int py) -> uint64_t {
                        return ((uint64_t)plane << 32)
                             | ((uint64_t)(uint16_t)(int16_t)px << 16)
                             | (uint16_t)(int16_t)py;
                    };
                    std::unordered_map<uint64_t, uint32_t> pos2idx;
                    pos2idx.reserve(npick);
                    for (uint32_t i = 0; i < npick; ++i)
                        pos2idx.emplace(poskey(im.plane_cells[i].plane,
                                               im.plane_cells[i].screen_x,
                                               im.plane_cells[i].screen_y), i);
                    // Frecuencia por hash de ESTE frame — ordena las anclas.
                    std::unordered_map<uint64_t, uint32_t> freq;
                    freq.reserve(npick);
                    for (uint32_t i = 0; i < npick; ++i)
                        ++freq[im.plane_cells[i].hash];
                    std::unordered_set<uint64_t> sup_hashes;
                    std::vector<uint32_t> mi;
                    std::vector<uint32_t> order;
                    std::unordered_set<uint64_t> done;   // orígenes emitidos (por set)
                    // ANIMACIÓN (): el paso vigente de cada secuencia se
                    // resuelve UNA vez por frame y no por match — llamarlo
                    // dentro del bucle re-anclaría el reloj tantas veces como
                    // instancias haya en pantalla.
                    std::unordered_map<uint64_t, uint32_t> seq_step_now;
                    if (!im.plane_seqs.empty())
                        for (const auto& [qid, qd] : im.plane_seqs)
                            (void)qd, seq_step_now.emplace(qid, UINT32_MAX);
                    for (const auto& [sid, def] : im.plane_sets) {
                        if (def.members.empty()) continue;
                        // ¿Este set pertenece a una Animación? Si sí, lo que se
                        // dibuja es el asset del paso VIGENTE, no el suyo: es
                        // lo que permite que el HD tenga más fases que el
                        // original. Ante varias secuencias gana la primera del
                        // índice (ordenado → determinista).
                        const std::string* use_asset = &def.asset;
                        if (auto sq = im.set_to_seq.find(sid); sq != im.set_to_seq.end()
                                                            && !sq->second.empty()) {
                            const uint64_t qid = sq->second.front().first;
                            auto& cached = seq_step_now[qid];
                            if (cached == UINT32_MAX) cached = im.plane_seq_step(qid);
                            auto qd = im.plane_seqs.find(qid);
                            if (qd != im.plane_seqs.end() && cached != UINT32_MAX &&
                                cached < qd->second.assets.size()) {
                                const std::string& a = qd->second.assets[cached];
                                if (!a.empty()) {
                                    use_asset = &a;
                                } else {
                                    // Paso sin asset propio: el del SET que
                                    // referencia (la escalera que ya existe).
                                    auto ps = im.plane_sets.find(qd->second.steps[cached]);
                                    if (ps != im.plane_sets.end() && !ps->second.asset.empty())
                                        use_asset = &ps->second.asset;
                                }
                            }
                        }
                        order.resize(def.members.size());
                        for (uint32_t k = 0; k < (uint32_t)order.size(); ++k) order[k] = k;
                        auto f = [&](uint32_t k) {
                            auto it = freq.find(def.members[k].hash);
                            return it == freq.end() ? 0u : it->second;
                        };
                        std::stable_sort(order.begin(), order.end(),
                                         [&](uint32_t a, uint32_t b) { return f(a) < f(b); });
                        done.clear();
                        for (uint32_t anck : order) {
                            const auto& anc = def.members[anck];
                            for (uint32_t i = 0; i < npick; ++i) {
                                const PlaneCellHit& pc = im.plane_cells[i];
                                if (pc.plane != def.plane || pc.hash != anc.hash ||
                                    consumed[i]) continue;
                                const int ox = pc.screen_x - anc.cx * 8;
                                const int oy = pc.screen_y - anc.cy * 8;
                                if (done.count(poskey(def.plane, ox, oy))) continue;
                                mi.clear();
                                bool all = true;
                                for (const auto& m : def.members) {
                                    const int ex = ox + m.cx * 8;
                                    const int ey = oy + m.cy * 8;
                                    if (ex < 0 || ex >= sw || ey < 0 || ey >= sh)
                                        continue;   // fuera del área visible: excusado
                                    auto pit = pos2idx.find(poskey(def.plane, ex, ey));
                                    if (pit == pos2idx.end() ||
                                        im.plane_cells[pit->second].hash != m.hash ||
                                        consumed[pit->second]) { all = false; break; }
                                    mi.push_back(pit->second);
                                }
                                if (!all || mi.empty()) continue;
                                done.insert(poskey(def.plane, ox, oy));
                                for (uint32_t k : mi) consumed[k] = 1;
                                for (const auto& m : def.members) sup_hashes.insert(m.hash);
                                AytherSpriteSub s{};
                                std::snprintf(s.asset_path, sizeof(s.asset_path), "%s",
                                              use_asset->c_str());
                                // : el offset de re-anclaje del HUD. Con
                                // (0,0) —todo Objeto que ya cae dentro del área
                                // segura— el quad sale en el mismo píxel que
                                // antes, así que un proyecto sin autorar nada
                                // no cambia un byte.
                                s.screen_x = (int16_t)(ox + def.off_x);
                                s.screen_y = (int16_t)(oy + def.off_y);
                                s.w_tiles  = (uint8_t)(def.w_cells < 255 ? def.w_cells : 255);
                                s.h_tiles  = (uint8_t)(def.h_cells < 255 ? def.h_cells : 255);
                                // Tinte E1 del SET (fundido de paleta): con
                                // referencia autorada, el ancla aporta su línea
                                // CRAM y el quad se tinta live/ref por canal —
                                // el isologotipo del título de GA era invisible
                                // en el pre-fade (CRAM negra) y el HD quedaba a
                                // todo color encima (reporte 2026-08-19). Sin
                                // referencia: 0xFF = neutro, como siempre.
                                const bool has_ref = def.ref_rgb[0] | def.ref_rgb[1]
                                                   | def.ref_rgb[2];
                                s.palette  = has_ref ? (uint8_t)(pc.palette & 3) : 0xFF;
                                s.synth_pal = 0xFF;  // sin síntesis E1
                                if (has_ref)
                                    std::memcpy(s.ref_rgb, def.ref_rgb, 3);
                                setq[(pc.flags >> 2) & 1].push_back(s);
                                setq_plane[(pc.flags >> 2) & 1].push_back(def.plane);
                            }
                        }
                    }
                    // Supresión de los originales por identidad — se aplica en
                    // el PRÓXIMO produce (misma latencia aceptada que 0x105).
                    if (!sup_hashes.empty()) {
                        bool added = false;
                        if (!im.plane_tile_suppress_any)
                            std::memset(im.plane_tile_suppress_want, 0,
                                        sizeof(im.plane_tile_suppress_want));
                        for (uint32_t i = 0; i < n_plane_tiles; ++i) {
                            const PlaneTileOccurrence& o = im.plane_tile_occs[i];
                            if (o.plane > 2 || !sup_hashes.count(o.hash)) continue;
                            const uint32_t key = ((uint32_t)o.pattern << 2) | (o.palette & 3u);
                            im.plane_tile_suppress_want[o.plane * 1024u + (key >> 3)]
                                |= (1u << (key & 7u));
                            added = true;
                        }
                        if (added) im.plane_tile_suppress_any = true;
                    }
                }

                // ── Emisión por lane: 1×1 (celdas con asset NO consumidas) +
                // quads de set al final de su lane. Orden de planos A·B·Window
                // dentro de cada pase, como siempre.
                auto emit_pass = [&](int want_prio) {
                    for (uint32_t i = 0; i < npick && n < kMaxPlaneTileSubs; ++i) {
                        const PlaneCellHit& pc = im.plane_cells[i];
                        if ((int)((pc.flags >> 2) & 1) != want_prio) continue;
                        if (!consumed.empty() && consumed[i]) continue;
                        char asset[256];
                        // : con el subsistema Planes apagado no se emite el
                        // overlay 1×1. La celda original ya está dibujada
                        // debajo, así que la vuelta al original es inmediata y
                        // no hay nada que restaurar.
                        if (!im.sub_on(Subsystem::Planes) ||
                            !ayther_tile_sub_lookup(im.plane_sub.get(), pc.hash,
                                                    asset, sizeof(asset)))
                            continue;
                        AytherSpriteSub& s = im.plane_tile_subs[n];
                        std::memset(s.asset_path, 0, sizeof(s.asset_path));
                        std::snprintf(s.asset_path, sizeof(s.asset_path), "%s", asset);
                        s.screen_x = pc.screen_x; s.screen_y = pc.screen_y;
                        s.w_tiles = 1; s.h_tiles = 1;
                        s.palette = 0xFF;   // sin ancla de sprite (lane de plano)
                        im.plane_tile_flips[n] = (uint8_t)(pc.flags & 3u);
                        im.plane_tile_sub_plane[n] = pc.plane;
                        ++n;
                    }
                    for (size_t qi = 0; qi < setq[want_prio].size(); ++qi) {
                        if (n >= kMaxPlaneTileSubs) break;
                        im.plane_tile_subs[n]  = setq[want_prio][qi];
                        im.plane_tile_flips[n] = 0;
                        im.plane_tile_sub_plane[n] = setq_plane[want_prio][qi];
                        ++n;
                    }
                };
                emit_pass(0);
                n_plane_tile_hi = n;     // a partir de acá, alta prioridad (sobre sprites)
                emit_pass(1);
                n_plane_tile_subs = n;
                n_plane_cells     = npick;
                // R-5 (): persistir qué celdas CONSUMIERON los matchers
                // (Cuadro/Panorámica/sets) — el inventario marca `claimed` con
                // esto y el compose por elementos no dibuja el original (el
                // reemplazo de los canales de supresión: lo que no gana no se
                // emite). Paralelo a plane_cells[0,npick); los apéndices de
                // borde no participan de los matchers → claimed 0.
                im.plane_cell_claimed.assign(n_plane_cells, 0);
                for (uint32_t ci = 0; ci < npick && ci < consumed.size(); ++ci)
                    im.plane_cell_claimed[ci] = consumed[ci];
                // R-3 (): publicar las celdas parciales de borde como
                // APÉNDICE, recién DESPUÉS de la firma del Cuadro, los
                // matchers y los sets (todos iteran [0,npick) — agregar celdas
                // ahí cambiaría firmas ya autoradas). El inventario de escena
                // y el panel Capas ven el count extendido; flag bit3 = parcial.
                for (uint32_t bi = 0; bi < nborder && n_plane_cells < kMaxPlaneCells; ++bi)
                    im.plane_cells[n_plane_cells++] = im.plane_cells_border[bi];
                // …pero el apéndice SÍ tiene que poder quedar reclamado. Una
                // celda parcial del borde está dentro de la pantalla y la tira
                // la cubre igual (su quad se recorta al viewport); sin reclamo,
                // el compose emite el original ahí y queda una FRANJA del plano
                // viejo al costado de la Panorámica. Se marca directo sobre
                // plane_cell_claimed —que es del inventario, paralelo a
                // plane_cells— sin tocar `consumed` ni el rango [0,npick) del
                // que dependen las firmas ya autoradas.
                if (im.pano_valid && im.hd_enabled && n_plane_cells > npick) {
                    auto pit = im.panoramas.find(im.pano_id);
                    if (pit != im.panoramas.end() && !pit->second.asset.empty()) {
                        const Impl::PanoramaDef& pd = pit->second;
                        im.plane_cell_claimed.resize(n_plane_cells, 0);
                        for (uint32_t i = npick; i < n_plane_cells; ++i) {
                            const PlaneCellHit& pc = im.plane_cells[i];
                            if (pc.plane != pd.plane) continue;
                            const int32_t lx = (im.pano_cam_x + pc.screen_x) >> 3;
                            const int32_t ly = (im.pano_cam_y + pc.screen_y) >> 3;
                            auto hit = pd.by_pos.find(Impl::pano_key(lx, ly));
                            if (hit == pd.by_pos.end()) continue;
                            if (Impl::pano_pos_matches(hit->second, pc.hash,
                                                       pc.palette))
                                im.plane_cell_claimed[i] = 1;
                        }
                    }
                }
            }
        }
    }

    // -- Modo 3 (RAM anchoring): asignar los sprites SAT a entidades por su
    //    world_pos leído de la RAM, con la cámara del VDP capturada arriba. Se
    //    llama SIEMPRE que hay perfil (resolve limpia sus resultados a la entrada,
    //    así un frame sin cámara/sprites no publica instancias viejas).
    if (im.mode3.has_profile()) {
        const uint8_t* m3_ram = m3_have_cam ? im.runner.work_ram() : nullptr;
        im.mode3.resolve(m3_ram, m3_ram ? im.runner.work_ram_size() : 0,
                         m3_cam_x, m3_cam_y, m3_wpx, m3_hpx,
                         im.sprite_occs, n_sprite_occs);
    }

    // -- Animaciones C-S2: por la pose que el juego muestra este frame, el
    //    frame HD del sheet (sub-rect) en el bbox — Pop o tween geométrico.
    if (im.anim.clip_count() > 0)
        im.anim.resolve(im.sprite_occs, n_sprite_occs, im.frame_index);
    else if (im.anim.frame_count() > 0)
        im.anim.resolve(nullptr, 0, im.frame_index);   // limpiar resultados viejos

    // -- Script: feed occurrences, fire on_frame ----------------------------
    AytherScriptEnv* sc = im.script.get();
    if (sc) {
        ayther_script_update_tiles  (sc, im.tile_occs,   n_tile_occs);
        ayther_script_update_sprites(sc, im.sprite_occs, n_sprite_occs);
        ayther_script_update_audio  (sc, im.audio_occs,  n_audio_occs);
        ayther_script_on_frame(sc, im.runner.work_ram(), im.runner.work_ram_size());
    }

    // -- Apply Lua overrides (clear unconditionally so stale ones never linger)
    // : el gate de audio se evalua UNA vez por frame, con la misma RAM y
    // el mismo criterio de word-swap que los tiles. Sin gate no cuesta nada.
    if (im.audio_gate) {
        uint64_t blocked[64];
        const uint32_t nb = ayther_audio_gate_eval(
            im.audio_gate.get(), im.runner.work_ram(), im.runner.work_ram_size(),
            true, im.frame_index, blocked, 64);
        im.audio_gate_blocked.clear();
        for (uint32_t i = 0; i < nb && i < 64; ++i) im.audio_gate_blocked.insert(blocked[i]);
    }
    if (im.tile_sub) {
        // EM-2 (): mismo contrato que el lane de planos — condiciones del
        // catálogo evaluadas una vez por frame antes de resolver.
        ayther_tile_sub_begin_frame(
            im.tile_sub.get(), im.frame_index, im.runner.work_ram(),
            static_cast<uint32_t>(im.runner.work_ram_size()), true);
        ayther_tile_sub_clear_overrides(im.tile_sub.get());
        if (sc) {
            AytherTileOverride ov[kMaxOverrides];
            const uint32_t n = ayther_script_get_tile_overrides(sc, ov, kMaxOverrides);
            for (uint32_t i = 0; i < n; ++i)
                ayther_tile_sub_add_override(im.tile_sub.get(), ov[i].hash, ov[i].asset_path);
        }
        // Lab authoring assignments — applied last so they win (a script may still
        // re-override per context next frame).
        for (const auto& [hash, asset] : im.lab_tile_overrides)
            ayther_tile_sub_add_override(im.tile_sub.get(), hash, asset.c_str());
    }
    if (im.audio_sub) {
        ayther_audio_sub_clear_overrides(im.audio_sub.get());
        if (sc) {
            AytherAudioOverride ov[kMaxOverrides];
            const uint32_t n = ayther_script_get_audio_overrides(sc, ov, kMaxOverrides);
            for (uint32_t i = 0; i < n; ++i)
                ayther_audio_sub_add_override(im.audio_sub.get(), ov[i].hash, ov[i].asset_path);
        }
        // Lab authoring assignments — applied last so they win.
        for (const auto& [hash, asset] : im.lab_audio_overrides)
            ayther_audio_sub_add_override(im.audio_sub.get(), hash, asset.c_str());
    }
    if (im.sprite_sub) {
        ayther_sprite_sub_clear_overrides(im.sprite_sub.get());
        if (sc) {
            AytherSpriteOverride ov[kMaxOverrides];
            const uint32_t n = ayther_script_get_sprite_overrides(sc, ov, kMaxOverrides);
            for (uint32_t i = 0; i < n; ++i)
                ayther_sprite_sub_add_override(im.sprite_sub.get(), ov[i].hash, ov[i].asset_path);
        }
        // Lab authoring assignments — applied last so they win for the artist's
        // current selection (a script can still re-override per context next frame).
        for (const auto& [hash, def] : im.lab_sprite_overrides)
            ayther_sprite_sub_add_override_ref(im.sprite_sub.get(), hash,
                                               def.asset.c_str(), def.ref_rgb);
    }
    // Preview HD transitorio (Animar): re-inyectar las pose-overrides en vivo en el
    // pose_sub cada frame (clear + add) — se resuelven como pose-set más abajo, con
    // prioridad sobre el catálogo del pack, y NO se serializan.
    if (im.pose_sub) {
        ayther_pose_sub_clear_overrides(im.pose_sub.get());
        for (const auto& p : im.preview_pose_overrides) {
            const bool has_rel = p.rel_x.size() == p.hashes.size()
                              && p.rel_y.size() == p.hashes.size();
            const bool has_dim = p.dim_w.size() == p.hashes.size()
                              && p.dim_h.size() == p.hashes.size();
            const bool has_flip = p.mem_flips.size() == p.hashes.size();
            const uint8_t base_mirror = static_cast<uint8_t>(
                (p.flip_h ? 1u : 0u) | (p.flip_v ? 2u : 0u));
            if (!p.candidates.empty()) {
                // Paso 2 (): alimentar los candidatos por variante — el motor
                // elige el más próximo a la variante observada del ancla.
                std::vector<int8_t>      pal, hf, vf;
                std::vector<uint16_t>    slots;   // : identidad por contenido
                std::vector<uint64_t>    sig;
                std::vector<const char*> aptr;
                pal.reserve(p.candidates.size()); hf.reserve(p.candidates.size());
                vf.reserve(p.candidates.size());  aptr.reserve(p.candidates.size());
                slots.reserve(p.candidates.size()); sig.reserve(p.candidates.size());
                for (const auto& c : p.candidates) {
                    pal.push_back(c.palette); hf.push_back(c.hflip); vf.push_back(c.vflip);
                    slots.push_back(c.slots); sig.push_back(c.sig);
                    aptr.push_back(c.asset.c_str());
                }
                ayther_pose_sub_add_override_variants(
                    im.pose_sub.get(), p.hashes.data(),
                    has_rel ? p.rel_x.data() : nullptr,
                    has_rel ? p.rel_y.data() : nullptr,
                    has_dim ? p.dim_w.data() : nullptr,
                    has_dim ? p.dim_h.data() : nullptr,
                    has_flip ? p.mem_flips.data() : nullptr,
                    static_cast<uint32_t>(p.hashes.size()), p.bbox_w, p.bbox_h,
                    base_mirror, p.ref_rgb, &p.ref_line[0][0],
                    p.asset.c_str(), pal.data(), hf.data(), vf.data(),
                    slots.data(), sig.data(),
                    aptr.data(), static_cast<uint32_t>(aptr.size()),
                    p.mask.empty() ? nullptr : p.mask.c_str());
            } else {
                ayther_pose_sub_add_override(im.pose_sub.get(), p.hashes.data(),
                                             has_rel ? p.rel_x.data() : nullptr,
                                             has_rel ? p.rel_y.data() : nullptr,
                                             has_dim ? p.dim_w.data() : nullptr,
                                             has_dim ? p.dim_h.data() : nullptr,
                                             has_flip ? p.mem_flips.data() : nullptr,
                                             static_cast<uint32_t>(p.hashes.size()),
                                             p.bbox_w, p.bbox_h, base_mirror,
                                             p.ref_rgb, &p.ref_line[0][0],
                                             p.asset.c_str(),
                                             p.mask.empty() ? nullptr
                                                            : p.mask.c_str());
            }
        }
    }

    // -- Resolve substitutions ----------------------------------------------
    uint32_t n_tile_subs = 0;
    if (im.tile_sub && n_tile_occs > 0 && im.sub_on(Subsystem::Tiles))   // 
        n_tile_subs = ayther_tile_sub_resolve(
            im.tile_sub.get(), im.tile_occs, n_tile_occs, im.tile_subs, kMaxTileSubs);

    // E1 CROMÁTICO (fundido + cambio de color): promedio RGB por línea de
    // paleta desde la CRAM viva (canales de 3 bits, colores 1-15) + el
    // factor ESCALAR clásico (luma Rec.601 / peak-hold), que queda como
    // fallback para subs sin referencia autorada. color_ram(): CRAM
    // EMPAQUETADA (u16 LE por color, R=bits0-2 · G=3-5 · B=6-8 —
    // verificado en el render VRAM, NO el layout "con huecos").
    // FUERA del gate de sprites A PROPÓSITO: el tinte de la PANORÁMICA también
    // sale de acá, y con la transición sin sprites en pantalla (f87-158 de la
    // demo de GA) el gate lo dejaba RANCIO — el 0 del fundido previo (f33-86,
    // CRAM negra) quedaba pegado y la tira se dibujaba negra durante toda la
    // transición (reporte 2026-07-31).
    double  pal_rgb[4][3] = {};                       // 0..1 por canal
    uint8_t pal_factor[4] = { 255, 255, 255, 255 };   // escalar clásico
    bool    cram_ok = false;
    {
        const uint8_t* cram = im.cram_ptr();          // E-3
        const size_t   csz  = im.runner.color_ram_size();
        for (int p = 0; p < 4; ++p) {
            const size_t base = static_cast<size_t>(p) * 16u * 2u;
            if (base + 16 * 2 > csz) continue;
            cram_ok = true;
            double r = 0.0, g = 0.0, b = 0.0;
            for (int i = 1; i < 16; ++i) {   // saltear el 0 (transparente)
                const size_t ce = base + static_cast<size_t>(i) * 2u;
                const uint16_t v = static_cast<uint16_t>(cram[ce] | (cram[ce + 1] << 8));
                r += v & 7; g += (v >> 3) & 7; b += (v >> 6) & 7;
            }
            pal_rgb[p][0] = r / (15.0 * 7.0);
            pal_rgb[p][1] = g / (15.0 * 7.0);
            pal_rgb[p][2] = b / (15.0 * 7.0);
            const double luma = 0.299 * pal_rgb[p][0] + 0.587 * pal_rgb[p][1]
                              + 0.114 * pal_rgb[p][2];
            if (luma > im.pal_luma_peak[p]) im.pal_luma_peak[p] = luma;
            const double peak = im.pal_luma_peak[p];
            double f = peak > 1e-4 ? luma / peak : 1.0;
            if (f < 0.0) f = 0.0; else if (f > 1.0) f = 1.0;
            pal_factor[p] = static_cast<uint8_t>(f * 255.0 + 0.5);
        }
    }
    // : FUNDIDO de la Panorámica — el quad del rect completo se tiñe
    // por la luma CRAM viva contra la referencia de su definición: la tira
    // se apaga y enciende CON la escena (sin esto quedaba a todo color
    // sobre un fundido a negro — f33–86 de la demo).
    // Tope del tinte de la tira. El byte es Q2.6, así que el formato admite
    // hasta 255/64 ≈ 3.98; se corta antes a propósito — un amanecer o un flash
    // tienen que poder ACLARAR (el tope viejo era 1.0 y los volvía invisibles),
    // pero una referencia capturada en un frame oscuro no debería poder mandar
    // la tira a blanco puro.
    constexpr double kPanoTintMax    = 2.0;
    /// Piso de señal por canal para confiar en el cociente cromático.
    constexpr double kPanoChromaFloor = 0.02;

    im.pano_tint[0] = im.pano_tint[1] = im.pano_tint[2] = 64;
    if (!im.pano_subs.empty() && cram_ok) {
        auto pit = im.panoramas.find(im.pano_id);
        if (pit != im.panoramas.end()) {
            auto& d = pit->second;
            // Factor ESCALAR (comportamiento ): sigue siendo el fallback y
            // el valor que usan los canales sin señal cromática de referencia.
            double luma = 0.0;
            for (int p = 0; p < 4; ++p)
                luma += 0.299 * pal_rgb[p][0] + 0.587 * pal_rgb[p][1]
                      + 0.114 * pal_rgb[p][2];
            luma /= 4.0;
            // Captura de la REFERENCIA, acá y no en define_panorama: con la
            // tira matcheada la escena que se ve ES la suya — la CRAM le
            // pertenece por construcción. Peak-hold de luma (como
            // pal_luma_peak): el primer frame matcheado la fija y uno más
            // luminoso la re-fija, así el fade-in de la transición sube la
            // referencia con la escena hasta su nivel normal y los fundidos
            // posteriores dividen contra ese nivel.
            if (luma > d.ref_peak) {
                d.ref_peak = luma;
                if (luma >= 0.05) d.ref_luma = luma;
                double wsum = 0.0;
                double ch[3] = { 0, 0, 0 };
                for (int p = 0; p < 4; ++p) {
                    const double w = 0.299 * pal_rgb[p][0]
                                   + 0.587 * pal_rgb[p][1]
                                   + 0.114 * pal_rgb[p][2];
                    d.ref_w[p] = w;
                    wsum += w;
                    for (int c = 0; c < 3; ++c) ch[c] += w * pal_rgb[p][c];
                }
                if (wsum >= 0.05) {
                    d.ref_ch[0] = ch[0]; d.ref_ch[1] = ch[1]; d.ref_ch[2] = ch[2];
                    d.ref_chroma = true;
                }
            }
            double fl = luma / std::max<double>(d.ref_luma, 0.05);
            if (fl < 0.0) fl = 0.0; else if (fl > kPanoTintMax) fl = kPanoTintMax;

            if (d.ref_chroma) {
                // CROMÁTICO: cociente POR CANAL contra la referencia, con los
                // pesos por línea del momento de definir en los dos lados — así
                // el cociente mide cómo cambió el color de lo que de verdad
                // aporta a la tira, no el promedio de las cuatro líneas.
                double live_ch[3] = { 0, 0, 0 };
                for (int p = 0; p < 4; ++p)
                    for (int c = 0; c < 3; ++c)
                        live_ch[c] += d.ref_w[p] * pal_rgb[p][c];
                for (int c = 0; c < 3; ++c) {
                    // Canal sin señal en la referencia (un fondo sin nada de
                    // rojo): el cociente se dispararía por dividir por ~0. Ahí
                    // no hay croma que seguir — se usa el escalar, que es lo
                    // que este código hacía siempre.
                    double f = d.ref_ch[c] >= kPanoChromaFloor
                                   ? live_ch[c] / d.ref_ch[c]
                                   : fl;
                    if (f < 0.0) f = 0.0; else if (f > kPanoTintMax) f = kPanoTintMax;
                    im.pano_tint[c] = (uint8_t)std::min<double>(255.0, f * 64.0 + 0.5);
                }
            } else {
                im.pano_tint[0] = im.pano_tint[1] = im.pano_tint[2] =
                    (uint8_t)std::min<double>(255.0, fl * 64.0 + 0.5);
            }
        }
    }

    // Tinte E1 de la lane de PLANO (quads de SET con referencia autorada):
    // live/ref POR CANAL de la línea CRAM del ancla — la misma fórmula de las
    // poses (6199), sin fallback gris: un sub sin referencia queda neutro
    // (64/64/64), byte-exacto con el comportamiento previo. Es lo que hace que
    // el Objeto siga los fundidos de paleta (el isologotipo del título de GA
    // sobre la CRAM negra del pre-fade, reporte 2026-08-19).
    for (uint32_t s = 0; s < n_plane_tile_subs; ++s) {
        uint8_t* t = im.plane_tile_tint + (size_t)s * 3;
        t[0] = t[1] = t[2] = 64;
        const AytherSpriteSub& sub = im.plane_tile_subs[s];
        if (!cram_ok || sub.palette == 0xFF ||
            !(sub.ref_rgb[0] | sub.ref_rgb[1] | sub.ref_rgb[2])) continue;
        const double* live = pal_rgb[sub.palette & 3];
        for (int c = 0; c < 3; ++c) {
            const double f = live[c] * 255.0 / std::max<int>(sub.ref_rgb[c], 1);
            const double v = f * 64.0 + 0.5;   // Q2.6
            t[c] = static_cast<uint8_t>(v < 0.0 ? 0.0 : (v > 255.0 ? 255.0 : v));
        }
    }

    // Sprites: los POSE-SETS (multi-sprite, por FIRMA de hashes) resuelven
    // PRIMERO y reclaman sus miembros; los sprites sin reclamar resuelven
    // per-hash. Ambos escriben en sprite_subs (pose-sets primero).
    // (El paso de metasprites v1 — por anim_group_id — se retiró: superseded
    // por Poses, ver ROADMAP/limpieza.)
    uint32_t n_sprite_subs = 0;
    uint32_t n_claimed_total = 0;    // subs de pose-set (índices [0, n) en sprite_subs)
    bool     pose_matched = false;   // ¿una pose-override reclamó miembros este frame?
    if (n_sprite_occs > 0) {
        std::memset(im.sprite_claimed, 0, n_sprite_occs);   // claims de pose-sets
        uint32_t n_pose = 0;

        // : los pose-sets son el subsistema Metasprites. Apagarlo tiene que
        // saltear el RESOLVE entero y no filtrar después: el matcher RECLAMA
        // occurrences (`sprite_claimed`), así que resolver y descartar dejaría
        // esos sprites fuera también del per-sprite — apagar metasprites se
        // llevaría puestos los sprites sueltos que quedan debajo.
        if (im.pose_sub && im.sub_on(Subsystem::Metasprites)) {
            // Límite de la tolerancia off-screen = área visible del MODO vivo
            // (256/320 × 224/240): las occurrences desaparecen en el borde
            // visible, no en la holgura 336/240 del hasher.
            ayther_pose_sub_set_screen(im.pose_sub.get(),
                                       static_cast<uint16_t>(im.snap.w),
                                       static_cast<uint16_t>(im.snap.h));
            // : CRAM viva → estabilidad por línea + latch de firmas de
            // contenido (identidad por swap parcial). Copia local por
            // alineación (color_ram() expone bytes).
            {
                const uint8_t* cram = im.cram_ptr();          // E-3
                const size_t   csz  = im.runner.color_ram_size();
                if (cram && csz >= 128) {
                    uint16_t words[64];
                    std::memcpy(words, cram, sizeof(words));
                    ayther_pose_sub_set_cram(im.pose_sub.get(), words, 64);
                }
            }
            n_pose = ayther_pose_sub_resolve(
                im.pose_sub.get(), im.sprite_occs, n_sprite_occs,
                im.sprite_claimed, im.sprite_subs, kMaxSpriteOccs);
        }

        const uint32_t n_claimed_subs = n_pose;
        n_claimed_total = n_claimed_subs;
        pose_matched = (n_pose > 0);   // sólo entonces vale la pena el compose de pose

        // 3. Lista de occs SIN reclamar para el per-sprite.
        const AytherSpriteOccurrence* sprite_input = im.sprite_occs;
        uint32_t                      sprite_input_n = n_sprite_occs;
        if (n_claimed_subs > 0) {
            uint32_t n_free = 0;
            for (uint32_t i = 0; i < n_sprite_occs; ++i)
                if (!im.sprite_claimed[i]) im.sprite_occs_free[n_free++] = im.sprite_occs[i];
            sprite_input   = im.sprite_occs_free;
            sprite_input_n = n_free;
        }

        // 4. Per-sprite (por hash) sobre los no reclamados.
        uint32_t n_per_sprite = 0;
        if (im.sprite_sub && sprite_input_n > 0 && n_claimed_subs < kMaxSpriteOccs
            && im.sub_on(Subsystem::Sprites))   // 
            n_per_sprite = ayther_sprite_sub_resolve(
                im.sprite_sub.get(), sprite_input, sprite_input_n,
                im.sprite_subs + n_claimed_subs, kMaxSpriteOccs - n_claimed_subs);

        n_sprite_subs = n_claimed_subs + n_per_sprite;

        // CU-AN-11: flip observado por sub (auto-espejado del sheet base por el VDP-flip).
        // Match por posición+tamaño contra las occurrences (que llevan hflip/vflip del SAT);
        // VkSprite::draw usa el flip para cargar la textura pre-volteada. Los subs de
        // metasprite (multi-sprite) raramente matchean 1:1 → flip 0 (sin espejo, v1).
        // D3 (): un sub de SNAPSHOT (pose sin HD, hd=false) trae la cara
        // CANÓNICA horneada (write_pose_snapshot_vram dibuja la apariencia de la
        // captura) → si la instancia matcheó en un arreglo ESPEJADO (sub.mirror,
        // del pose_sub), se dibuja pre-volteado por esos bits: Tyris caminando a
        // la derecha se ve a la derecha aunque la pose se capturó a la izquierda.
        // mirror==0 → lógica previa: snapshot tal cual (flip horneado, sin doble
        // espejo — el "reloj desfasado") y sólo el HD AUTORADO (hd=true) toma el
        // flip de su occ exacta (orientación canónica del artista).
        for (uint32_t s = 0; s < n_sprite_subs; ++s) {
            const AytherSpriteSub& sub = im.sprite_subs[s];
            if (s < n_claimed_subs && sub.mirror) {
                im.sprite_sub_flips[s] = static_cast<uint8_t>(sub.mirror & 3);
                continue;
            }
            bool baked = false;   // snapshot (hd=false) → flip ya horneado
            for (const auto& pv : im.preview_pose_overrides)
                if (!pv.hd && pv.asset == sub.asset_path) { baked = true; break; }
            uint8_t flip = 0;
            if (!baked)
                for (uint32_t o = 0; o < n_sprite_occs; ++o) {
                    const AytherSpriteOccurrence& oc = im.sprite_occs[o];
                    if (oc.screen_x == sub.screen_x && oc.screen_y == sub.screen_y
                        && oc.w_tiles == sub.w_tiles && oc.h_tiles == sub.h_tiles) {
                        flip = static_cast<uint8_t>((oc.hflip & 1) | ((oc.vflip & 1) << 1));
                        break;
                    }
                }
            im.sprite_sub_flips[s] = flip;
        }

        // C8 (z-order entre HD superpuestos): slot por sub; el renderer los ordena
        // por slot DESCENDENTE (menor = al frente, dibujado último en el painter's).
        //  - Sub CLAIMED (pose): slot = menor slot SAT de las occs reclamadas
        //    MIEMBRO de su pose (centro en el rect + hash del pose-set) — el orden
        //    REAL del hardware entre personajes solapados (reporte 2026-07-14: la
        //    amazona montada va DELANTE del dragón; el ÁREA de antes lo invertía
        //    cuando la pose de adelante era más grande — Tyris Ride 42 tiles vs
        //    dragón echado 40 → el dragón quedaba encima). El caso que motivó el
        //    área — accesorio CONTENIDO (el reloj dentro del bbox del Genio: slot
        //    SAT mayor pero visible por transparencia del original) — lo conserva
        //    la pasada de contención de abajo. Sin miembro identificable → área.
        //  - Sub PER-SPRITE: el slot SAT de SU occ (match exacto por posición+tamaño,
        //    como flips) o el menor slot de las occs del bbox. 255 si nada matchea.
        for (uint32_t s = 0; s < n_sprite_subs; ++s) {
            const AytherSpriteSub& sub = im.sprite_subs[s];
            if (s < n_claimed_subs) {
                const uint8_t mn = sub_member_min_slot(
                    sub, im.sprite_occs, n_sprite_occs, im.sprite_claimed,
                    im.preview_pose_overrides);
                im.sprite_sub_slot[s] = mn != 255 ? mn
                    : (uint8_t)std::min<int>((int)sub.w_tiles * (int)sub.h_tiles, 255);
                continue;
            }
            uint8_t exact = 255, overlap = 255;
            const int sx0 = sub.screen_x, sy0 = sub.screen_y;
            const int sx1 = sx0 + sub.w_tiles * 8, sy1 = sy0 + sub.h_tiles * 8;
            for (uint32_t o = 0; o < n_sprite_occs; ++o) {
                const AytherSpriteOccurrence& oc = im.sprite_occs[o];
                if (oc.screen_x == sub.screen_x && oc.screen_y == sub.screen_y
                    && oc.w_tiles == sub.w_tiles && oc.h_tiles == sub.h_tiles) {
                    exact = oc.slot; break;                 // per-sprite: su propio slot
                }
                const int ox1 = oc.screen_x + oc.w_tiles * 8;
                const int oy1 = oc.screen_y + oc.h_tiles * 8;
                if (oc.screen_x < sx1 && ox1 > sx0 && oc.screen_y < sy1 && oy1 > sy0)
                    overlap = std::min<uint8_t>(overlap, oc.slot);   // fallback: frontmost del bbox
            }
            im.sprite_sub_slot[s] = (exact != 255) ? exact : overlap;
        }
        // PRIORIDAD VDP por sub: la del occ EXACTO (posición+tamaño) o la del
        // occ frontmost del bbox — la misma occ de la que sale el slot. El
        // hardware ordena sprite pri-1 > plano A pri-1 (las letras del título
        // de GA giran alrededor del isologotipo conmutando este bit): el
        // renderer necesita saberlo POR FRAME para poner el HD del sprite
        // delante o detrás del Primer plano HD.
        for (uint32_t s = 0; s < n_sprite_subs; ++s) {
            const AytherSpriteSub& sub = im.sprite_subs[s];
            uint8_t pr = 0, best_slot = 255;
            const int sx0 = sub.screen_x, sy0 = sub.screen_y;
            const int sx1 = sx0 + (sub.w_px ? sub.w_px : sub.w_tiles * 8);
            const int sy1 = sy0 + (sub.h_px ? sub.h_px : sub.h_tiles * 8);
            for (uint32_t o = 0; o < n_sprite_occs; ++o) {
                const AytherSpriteOccurrence& oc = im.sprite_occs[o];
                if (oc.screen_x == sub.screen_x && oc.screen_y == sub.screen_y
                    && oc.w_tiles == sub.w_tiles && oc.h_tiles == sub.h_tiles) {
                    pr = oc.priority & 1; best_slot = 0; break;
                }
                const int ox1 = oc.screen_x + oc.w_tiles * 8;
                const int oy1 = oc.screen_y + oc.h_tiles * 8;
                if (oc.screen_x < sx1 && ox1 > sx0 && oc.screen_y < sy1 &&
                    oy1 > sy0 && oc.slot < best_slot) {
                    best_slot = oc.slot;
                    pr = oc.priority & 1;
                }
            }
            im.sprite_sub_prio[s] = pr;
        }

        // Pasada de CONTENCIÓN (el reloj dentro del Genio): un sub reclamado cuyo
        // rect queda totalmente DENTRO del rect de otro es un accesorio ENCIMA —
        // su slot SAT es mayor (el hardware lo dibuja detrás, visible por
        // transparencia del original) pero el HD del contenedor es opaco → se lo
        // fuerza justo DELANTE del contenedor. Solapamiento PARCIAL (dos
        // personajes) queda en el orden SAT real de arriba.
        for (uint32_t a = 0; a < n_claimed_subs; ++a)
            for (uint32_t b = 0; b < n_claimed_subs; ++b) {
                if (a == b) continue;
                const AytherSpriteSub& A = im.sprite_subs[a];
                const AytherSpriteSub& B = im.sprite_subs[b];
                const int aw = A.w_px ? A.w_px : A.w_tiles * 8;
                const int ah = A.h_px ? A.h_px : A.h_tiles * 8;
                const int bw = B.w_px ? B.w_px : B.w_tiles * 8;
                const int bh = B.h_px ? B.h_px : B.h_tiles * 8;
                const bool inside = A.screen_x >= B.screen_x && A.screen_y >= B.screen_y
                                 && A.screen_x + aw <= B.screen_x + bw
                                 && A.screen_y + ah <= B.screen_y + bh
                                 && (aw < bw || ah < bh);
                if (inside && im.sprite_sub_slot[a] >= im.sprite_sub_slot[b])
                    im.sprite_sub_slot[a] = im.sprite_sub_slot[b] > 0
                        ? (uint8_t)(im.sprite_sub_slot[b] - 1) : 0;
            }

        // E3 (Shadow/Highlight): si el VDP corre en modo S/H (reg 0x0C bit 3) y
        // el sprite es de PRIORIDAD BAJA, el hardware lo dibuja a media luz →
        // se compone ×0.5 sobre el factor de E1 (MISMO canal — el shader ya hace
        // rgb*=luma). Highlight (operadores color 14/15) = fuera de alcance v1.
        // La regla exacta del hardware es por-píxel (depende de la prioridad del
        // FONDO detrás); v1 aproxima con la prioridad del sprite = caso común
        // ("sprite en sombra por prioridad"). VALIDADO en positivo contra
        // Vectorman (, 2026-07-15): `sh_probe --validate-e3` — low-pri bajo
        // S/H tinta exacto 32/32/32 vs 64/64/64 del high-pri, y sin S/H no hay
        // sombra espuria. La aproximación por-sprite queda documentada; si un
        // caso real difiere (fondo de otra prioridad detrás), issue nuevo.
        const uint8_t* sh_regs = im.regs_ptr();       // E-3
        const size_t   sh_rsz  = im.runner.vdp_regs_size();
        const bool sh_mode = sh_regs && sh_rsz > 0x0C && (sh_regs[0x0C] & 0x08) != 0;
        for (uint32_t s = 0; s < n_sprite_subs; ++s) {
            const AytherSpriteSub& sub = im.sprite_subs[s];
            // La paleta del ANCLA viaja en el sub desde el core (la occ del
            // per-sprite; el miembro ancla de la pose). La heurística vieja
            // ("primera occ cuyo centro cae en el bbox") tomaba la paleta de un
            // AJENO solapado — Tyris montada (paleta 0, constante) dentro del
            // bbox del Dragón (paleta 2) → el flash de paleta del juego nunca
            // modulaba el HD del dragón (f1478 de «Demo Amazona»). Queda como
            // fallback sólo para subs sin ancla (palette == 0xFF).
            const bool claimed  = (s < n_claimed_total);
            const bool anchored = sub.palette != 0xFF;
            // Extent EXACTO en px (bbox de pose no tile-múltiplo): 0 = tiles×8.
            const int sx1 = sub.screen_x + (sub.w_px ? sub.w_px : sub.w_tiles * 8);
            const int sy1 = sub.screen_y + (sub.h_px ? sub.h_px : sub.h_tiles * 8);
            // Tinte RGB (1.0 = neutro). Prioridad de referencia:
            //     — asset autorado en OTRA línea (synth_pal): razón viva
            //           observada/candidata (ambas líneas están en CRAM ahora).
            //   ref   — referencia AUTORADA de la pose (promedio RGB al
            //           capturar): live/ref sigue fades Y flashes de color,
            //           incluso >1 (flash más brillante que lo normal).
            //   gris  — sin referencia: factor escalar clásico (luma/peak).
            double t[3] = { 1.0, 1.0, 1.0 };
            uint8_t grey = anchored ? pal_factor[sub.palette & 3] : 255;
            bool use_grey = true;
            if (anchored && cram_ok) {
                const double* live = pal_rgb[sub.palette & 3];
                if (sub.synth_pal != 0xFF) {
                    const double* ref = pal_rgb[sub.synth_pal & 3];
                    for (int c = 0; c < 3; ++c)
                        t[c] = live[c] / (ref[c] > 1e-3 ? ref[c] : 1e-3);
                    use_grey = false;
                } else if (sub.ref_rgb[0] | sub.ref_rgb[1] | sub.ref_rgb[2]) {
                    for (int c = 0; c < 3; ++c)
                        t[c] = live[c] * 255.0 / std::max<int>(sub.ref_rgb[c], 1);
                    use_grey = false;
                }
            }
            bool shadow = false;
            for (uint32_t o = 0; o < n_sprite_occs; ++o) {
                const AytherSpriteOccurrence& oc = im.sprite_occs[o];
                bool hit;
                if (claimed) {
                    // E3: la sombra también se decide sobre un MIEMBRO (misma
                    // paleta que el ancla), no sobre el ajeno solapado.
                    const int cx = oc.screen_x + oc.w_tiles * 4, cy = oc.screen_y + oc.h_tiles * 4;
                    hit = cx >= sub.screen_x && cx < sx1 && cy >= sub.screen_y && cy < sy1
                       && (!anchored || (oc.palette & 3) == (sub.palette & 3));
                } else {
                    hit = oc.screen_x == sub.screen_x && oc.screen_y == sub.screen_y
                       && oc.w_tiles == sub.w_tiles && oc.h_tiles == sub.h_tiles;
                }
                if (hit) {
                    if (!anchored) grey = pal_factor[oc.palette & 3];
                    shadow = sh_mode && oc.priority == 0;   // sombra: media luz
                    break;
                }
            }
            if (use_grey)
                t[0] = t[1] = t[2] = grey / 255.0;
            for (int c = 0; c < 3; ++c) {
                double v = t[c] * (shadow ? 0.5 : 1.0) * 64.0 + 0.5;   // Q2.6
                im.sprite_sub_tint[s * 3 + c] =
                    static_cast<uint8_t>(v < 0.0 ? 0.0 : (v > 255.0 ? 255.0 : v));
            }
        }
    }

    // CU-AN in-betweens: el TweenPlayer filtra el HD ya resuelto (playback POR TIEMPO
    // del dibujo intermedio antes de sostener el keyframe). begin_frame avanza el timer
    // 1 vez/frame; resolve sustituye el asset del sub por el intermedio en curso. Los
    // assets sin secuencia de tween pasan tal cual (no pisan el estado del personaje).
    if (im.tween) {
        ayther_tween_begin_frame(im.tween.get());
        char tw[256];
        for (uint32_t s = 0; s < n_sprite_subs; ++s) {
            // v2: identidad de la INSTANCIA = pose_key del sub + centro del
            // bbox — el TweenPlayer trackea cada personaje por separado.
            const AytherSpriteSub& sb = im.sprite_subs[s];
            const int32_t cw = sb.w_px ? sb.w_px : sb.w_tiles * 8;
            const int32_t ch = sb.h_px ? sb.h_px : sb.h_tiles * 8;
            ayther_tween_resolve(im.tween.get(), sb.asset_path, sb.pose_key,
                                 sb.screen_x + cw / 2, sb.screen_y + ch / 2,
                                 tw, sizeof(tw));
            std::memcpy(im.sprite_subs[s].asset_path, tw, sizeof(tw));
        }
    }

    uint32_t n_audio_subs = 0;
    if (im.audio_sub && n_audio_occs > 0)
        n_audio_subs = ayther_audio_sub_resolve(
            im.audio_sub.get(), im.audio_occs, n_audio_occs, im.audio_subs, kMaxAudioOccs);

    // -- HD audio out (motor-owned): play -> refresh mute set -> flush PCM ----
    // Order matters: the mute set must be known before flush so a substituted
    // hash is muted on its first appearance (no 1-tick bleed — v0.9.7).
    // replay_quiet: durante el warm/bake (migración) no se reproduce nada — se
    // descarta el PCM staged en vez de mandarlo al device. audio_audible: idem
    // para los produce internos de la app (cargar tomas/poses hace seeks y
    // re-produces que "chillaban" un frame de audio — reporte 2026-07-24);
    // audible solo reproduciendo o con el usuario scrubeando un timeline.
    // : barrido de one-shots con tail FINITO — el mismo límite que dejó
    // de mutear el original corta el HD (fade rápido del player). Corre
    // también con la salida inaudible: la ventana expira aunque no se oiga.
    if (im.audio_enabled && !im.hd_oneshot_cut.empty()) {
        for (auto it = im.hd_oneshot_cut.begin();
             it != im.hd_oneshot_cut.end(); ) {
            if (im.frame_index > it->second) {
                im.audio.stop_sfx_by_key(it->first);
                it = im.hd_oneshot_cut.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (im.audio_enabled && !im.replay_quiet && im.audio_audible) {
        // : transacción también en las subs por HASH — mutear un batch y
        // que su asset no decodifique era silencio garantizado. El play y el
        // set_mute_hashes consumen la MISMA lista filtrada por disponibilidad:
        // lo que no puede sonar no se muta, y el original pasa.
        uint32_t n_ready_subs = 0;
        for (uint32_t i = 0; i < n_audio_subs; ++i) {
            const AytherAudioSub& s = im.audio_subs[i];
            // (Sin contador acá: la resolución es POR FRAME y un asset roto
            // sumaría 60/s — hd_fallback cuenta ocurrencias, no frames.)
            if (s.asset_path[0] == '\0' ||
                !im.audio.asset_ready_pack(im.pack.get(), s.asset_path))
                continue;
            im.audio_subs_ready[n_ready_subs++] = s;
        }
        if (n_ready_subs > 0)
            im.audio.play_substitutions(im.pack.get(), im.audio_subs_ready,
                                        n_ready_subs);
        im.audio.set_mute_hashes(im.audio_subs_ready, n_ready_subs);
        // C-A2: sustitución por EVENTO — dentro de un evento sustituido se
        // descarta el PCM del emulador ENTERO (rango-mute: sus batches cambian
        // de hash, no se puede mutear por hash) y en el start_frame arranca el
        // asset HD en fase. Los frames de los eventos son frames de la TOMA →
        // matchean durante su replay (frame_index == frame de la toma).
        // La cuenta de muestras del emulador se lee ANTES del flush: flush lo
        // vacía, y leerla después daba SIEMPRE 0 — con lo cual el sintetizador
        // caía al número fijo y el arreglo quedaba inerte. Se pasa a synth_tick.
        im.synth_frames_hint = im.audio.pending_frames();
        // El router mezcla en el PCM del emulador, así que corre ANTES del
        // flush. synth_tick va primero para dejarle su bloque listo (con el
        // router apagado sigue encolando por su cuenta, como siempre).
        im.synth_tick();   // : la voz sintetizada del timbre asignado
        // : el router de canales por voz. `routed` = dejó su bloque pisando
        // lo staged; si no rindió (re-producir el MISMO frame), lo staged sigue
        // siendo del chip.
        const bool routed = im.voice_tick();
        if (im.voice_router_on) {
            // El range-mute por evento no aplica: existía para tapar el PCM del
            // emulador debajo de un asset HD, y ahora esa voz ya calla en el
            // router (SessionPolicy).
            //
            // Sin bloque del router, lo staged es el chip CRUDO y empujarlo
            // sería oír el original por debajo — el chip ya no viene muteado del
            // core, que es lo que deja al hasher ver el audio de verdad.
            //  unificado: el discard tiraría TAMBIÉN la mezcla HD del
            // bloque — flush(true) silencia el original y conserva el eje de
            // tiempo con las voces adentro.
            //
            // : en Sega CD lo staged NO se silencia nunca, ni siquiera sin
            // bloque del router — es el chip PCM y el CDDA, y ya viene del core
            // con los canales del router callados por máscara. Silenciarlo era
            // justamente lo que dejaba el sistema mudo.
            if (routed || im.router_mix()) im.audio.flush_emulator();
            else                           im.audio.flush_emulator(true);
        } else if (im.pack_evt_mute_at(im.frame_index)) {   // : transaccional
            // : ídem — el range-mute calla el original, no la mezcla HD
            // que viaja en el mismo bloque. (: el `discard_emulator()` del
            // camino viejo tiraba las dos cosas.)
            im.audio.flush_emulator(true);
        } else {
            im.audio.flush_emulator();
        }
        AudioEventTrigger trg[4];
        for (uint32_t i = 0, k = im.audio_evt.triggers_at(im.frame_index, trg, 4); i < k; ++i) {
            // Silenciado (): el catálogo del pack dispara por firma, y el
            // altavoz del Lab tiene que alcanzarlo igual que a los otros dos
            // caminos — si no, silenciar un sonido con el pack cargado sigue
            // dejando sonar su asset.
            if (im.signature_muted(trg[i].signature, 0)) { ++im.hd_muted; continue; }
            // : el resultado cierra la transacción — un fallo saca la
            // firma del range-mute (pack_evt_mute_at) y el original pasa.
            // : cut_frame = end + tail de la política (ilimitado si el
            // pack no lo declara — legacy).
            im.hd_fired(trg[i].signature,
                        im.audio.play_event_hd(im.pack.get(), trg[i].asset,
                                               trg[i].looping, trg[i].signature,
                                               trg[i].end_frame,
                                               im.cut_frame_of(trg[i].signature,
                                                               trg[i].end_frame),
                                               0.0,
                                               im.fade_of(trg[i].signature)));
        }
        im.audio.tick_events(im.frame_index);
        im.audio.tick();   // reap finished one-shot SFX streams
        ++im.aud_n_flushed;
    } else if (im.audio_enabled) {
        im.audio.discard_emulator();
        // Por QUÉ se descartó. Las tres compuertas son distintas y tienen
        // arreglos distintos, pero desde afuera «no se escucha nada» se ve
        // igual en los tres casos — y ese era exactamente el problema para
        // diagnosticar el mudo de Capturar. Contarlas por separado es lo que
        // convierte el síntoma en una respuesta.
        if (im.replay_quiet)         ++im.aud_n_quiet;
        else if (!im.audio_audible)  ++im.aud_n_inaudible;
    } else {
        ++im.aud_n_disabled;
    }

    // -- Publish the FrameView ----------------------------------------------
    FrameView& v = im.view;
    v = FrameView{};
    v.fb_pixels = im.snap.data;
    v.fb_width  = im.snap.w;
    v.fb_height = im.snap.h;
    v.fb_pitch  = static_cast<uint32_t>(im.snap.pitch);
    v.fb_format = static_cast<int>(im.runner.pixel_format());

    v.tile_subs        = im.tile_subs;    v.tile_sub_count   = n_tile_subs;
    v.sprite_subs      = im.sprite_subs;  v.sprite_sub_count = n_sprite_subs;
    v.sprite_sub_flips = im.sprite_sub_flips;   // CU-AN-11
    v.sprite_sub_tint  = im.sprite_sub_tint;    // E1 cromático (fundido + color)
    v.sprite_sub_slot  = im.sprite_sub_slot;    // C8 (z-order SAT)
    v.sprite_sub_prio  = im.sprite_sub_prio;    // bit VDP: pri-1 delante del plano HI
    v.plane_tile_subs  = im.plane_tile_subs;  v.plane_tile_sub_count = n_plane_tile_subs;
    v.plane_tile_flips = im.plane_tile_flips; v.plane_tile_sub_hi    = n_plane_tile_hi;
    v.plane_tile_sub_tint = im.plane_tile_tint;   // E1 (sets con ref autorada)
    v.tile_occs        = im.tile_occs;    v.tile_occ_count   = n_tile_occs;
    v.sprite_occs      = im.sprite_occs;  v.sprite_occ_count = n_sprite_occs;
    v.audio_occs       = im.audio_occs;   v.audio_occ_count  = n_audio_occs;
    v.chip_writes      = im.chip_writes.data();
    v.chip_write_count = static_cast<uint32_t>(im.chip_writes.size());
    v.audio_mute_mask  = im.audio_mute_applied;
    v.audio_active_subs      = im.audio_active_subs.empty() ? nullptr : im.audio_active_subs.data();
    v.audio_active_sub_count = static_cast<uint32_t>(im.audio_active_subs.size());
    v.plane_a_count    = n_plane_a;       v.plane_b_count    = n_plane_b;
    v.plane_w_count    = n_plane_w;
    v.plane_tile_occs  = im.plane_tile_occs;  v.plane_tile_occ_count = n_plane_tiles;
    v.plane_cells      = im.plane_cells;       v.plane_cell_count     = n_plane_cells;
    v.entity_subs      = im.mode3.subs();      v.entity_sub_count      = im.mode3.sub_count();
    v.entity_instances = im.mode3.instances(); v.entity_instance_count = im.mode3.instance_count();
    v.anim_frames      = im.anim.frames();     v.anim_frame_count      = im.anim.frame_count();
    for (int p = 0; p < 3; ++p) { v.plane_hscroll[p] = plane_hsc[p]; v.plane_vscroll[p] = plane_vsc[p]; }
    v.plane_wpx = plane_wpx_v; v.plane_hpx = plane_hpx_v;
    v.vs_two_cell = vs_two_v;
    for (int p = 0; p < 2; ++p)
        for (int c2c = 0; c2c < 20; ++c2c)
            v.plane_vscroll_col[p][c2c] = plane_vsc_col[p][c2c];
    for (int p = 0; p < 3; ++p) {
        v.screen_plane_sig[p]   = scr_sig[p];
        v.screen_plane_cells[p] = scr_cells[p];
    }
    v.screen_match_id    = im.screen_active;
    v.screen_match_score = im.screen_score;
    v.screen_match_extra = im.screen_extra;
    v.screen_presence_count = im.screen_presence_n;
    for (uint32_t i = 0; i < im.screen_presence_n; ++i)
        v.screen_presence_ids[i] = im.screen_presence[i];
    v.screen_subs        = im.screen_sub_n ? &im.screen_sub : nullptr;
    v.screen_sub_count   = im.screen_sub_n;
    v.kinematic_id       = im.kine_active;
    v.kinematic_step     = im.kine_step;
    {
        auto kt = im.kinematics.find(im.kine_active);
        v.kinematic_steps = kt != im.kinematics.end()
                          ? (uint32_t)kt->second.steps.size() : 0u;
    }
    // VIDEO del paso (). La sesión
    // produce píxeles, el renderer los sube.
    // : la máscara de planos del video — la UNIÓN de las de sus Cuadros,
    // calculada acá y no por paso. Con la del paso vigente el video cambiaría
    // de posición en el stack a mitad de reproducción (un paso queda debajo de
    // B y el siguiente encima): ese parpadeo de z se ve como un glitch, y
    // cubrir un plano de más se ve como una decisión. El precio, asumido: un
    // solo paso que declare Window hace que el video tape Window toda la
    // Cinemática.
    v.wide_w = im.wide_w_eff;   //  fase 0 + gate EM-8.2
    v.video_plane_mask = 0;
    if (im.vid_on && im.kine_active) {
        if (const auto kt = im.kinematics.find(im.kine_active);
            kt != im.kinematics.end())
            for (uint64_t sid : kt->second.steps)
                if (const auto sc = im.screens.find(sid); sc != im.screens.end())
                    v.video_plane_mask |= sc->second.mask;
    }
    //  decisión 3: la PRIORIDAD la decide el dato del VDP, no un flag de
    // autoría. Si las celdas vivas de los planos que el video cubre son
    // mayoritariamente pri-1, el Cuadro es un primer plano (el flash a pantalla
    // completa de una cinemática) y el video tiene que ir al frente: puesto en
    // el z del fondo quedaría DETRÁS de lo que justamente viene a cubrir.
    //
    // Se cuenta sobre `plane_cells` —las celdas REALES de este frame, con el
    // bit de prioridad del word de nametable (bit2 de `flags`)— y no sobre la
    // definición del Cuadro, que guarda hashes por posición y no prioridad.
    // Convención de la máscara: bit0=A · bit1=B · bit2=Window ↔ plane 0=A ·
    // 1=B · 2=Window.
    v.video_front = 0;
    if (v.video_plane_mask) {
        uint32_t hi = 0, tot = 0;
        for (uint32_t i = 0; i < v.plane_cell_count; ++i) {
            const PlaneCellHit& pc = v.plane_cells[i];
            if (pc.plane > 2 || !(v.video_plane_mask & (1u << pc.plane))) continue;
            ++tot;
            if (pc.flags & 0x04) ++hi;
        }
        v.video_front = (tot && hi * 2 > tot) ? 1 : 0;
    }
    v.video_y        = im.vid_on ? im.vid_out.y.data : nullptr;
    v.video_u        = im.vid_on ? im.vid_out.u.data : nullptr;
    v.video_v        = im.vid_on ? im.vid_out.v.data : nullptr;
    v.video_y_stride = im.vid_on ? im.vid_out.y.stride : 0u;
    v.video_u_stride = im.vid_on ? im.vid_out.u.stride : 0u;
    v.video_v_stride = im.vid_on ? im.vid_out.v.stride : 0u;
    v.video_w      = im.vid_on ? im.vid_out.w : 0u;
    v.video_h      = im.vid_on ? im.vid_out.h : 0u;
    v.video_frame  = im.vid_on ? im.vid_out.index : 0u;
    v.video_seq    = im.vid_on ? im.vid_out.seq : 0ull;

    v.panorama_cam_x = im.pano_cam_x;
    v.panorama_cam_y = im.pano_cam_y;
    v.panorama_id    = im.pano_id;
    v.panorama_votes = im.pano_votes;
    v.panorama_cells = im.pano_cells;
    v.panorama_cover = im.pano_cover;
    v.panorama_valid = im.pano_valid;
    // : la limpieza de la tira anclada. `clean_pct` se calculaba desde
    // EM-8.1 y no salía de la sesión — sin publicarlo, «cobertura 100 %» era la
    // única cifra a la vista y decía lo que no es.
    v.panorama_clean = 0;
    if (im.pano_valid) {
        auto cit = im.panoramas.find(im.pano_id);
        if (cit != im.panoramas.end()) v.panorama_clean = cit->second.clean_pct;
    }
    v.panorama_subs       = im.pano_subs.empty() ? nullptr : im.pano_subs.data();
    v.panorama_sub_count  = (uint32_t)im.pano_subs.size();
    v.panorama_plane      = 0;
    v.panorama_sub_tint   = im.pano_subs.empty() ? nullptr : im.pano_tint;
    if (!im.pano_subs.empty()) {
        auto pit = im.panoramas.find(im.pano_id);
        if (pit != im.panoramas.end()) v.panorama_plane = pit->second.plane;
    }

    // ── EM-1: cámara de NIVEL — unwrap del delta de scroll (mod wpx/hpx),
    // solo con avance SECUENCIAL de frame; un salto re-ancla en 0 y baja el
    // flag hasta el próximo frame consecutivo. Re-produce del MISMO frame
    // (pausa/invalidate) no toca el tracker. La cámara se mueve OPUESTA al
    // scroll (mismo unwrap que el stitcher).
    {
        const bool same = im.cam_last_frame == im.frame_index;
        const bool seq  = im.cam_last_frame != UINT64_MAX &&
                          im.frame_index == im.cam_last_frame + 1;
        if (!same) {
            if (seq) {
                // El scroll hay que des-wrapearlo en el módulo en que REALMENTE
                // wrapea, que es el ancho del plano: sólo `H mod wpx` decide la
                // imagen (`sx = (cx*8 + H) mod wpx`). Pero el campo del VDP es
                // de 10 bits y el juego puede escribir valores por encima de
                // wpx, así que hay que REDUCIR primero y recién ahí unwrapear.
                //   · Sonic 2 llega a H=1023 con wpx=512: 1023→0 es +1.
                //   · Aladdin se queda en [0,512): 510→1 es +3.
                // Cualquiera de los dos módulos a secas acierta en un juego y
                // falla en el otro (el módulo 0x400 metía un salto de 509 px en
                // Aladdin); reducir y unwrapear en wpx acierta en los dos.
                const int wrapw = plane_wpx_v ? plane_wpx_v : 512;
                const int wraph = plane_hpx_v ? plane_hpx_v : 512;
                auto unwrap = [](int cur, int prev, int m) {
                    int d = (cur % m) - (prev % m);
                    if (d >  m / 2) d -= m;
                    else if (d < -m / 2) d += m;
                    return d;
                };
                for (int p = 0; p < 2; ++p) {
                    im.cam_x[p] += -unwrap(plane_hsc[p], im.cam_prev_h[p], wrapw);
                    im.cam_y[p] += -unwrap(plane_vsc[p], im.cam_prev_v[p], wraph);
                }
                im.cam_valid = true;
            } else {
                for (int p = 0; p < 2; ++p) { im.cam_x[p] = 0; im.cam_y[p] = 0; }
                im.cam_valid = false;
            }
            for (int p = 0; p < 2; ++p) {
                im.cam_prev_h[p] = plane_hsc[p];
                im.cam_prev_v[p] = plane_vsc[p];
            }
            im.cam_last_frame = im.frame_index;
        }
    }
    for (int p = 0; p < 2; ++p) {
        v.plane_cam_x[p] = im.cam_x[p];
        v.plane_cam_y[p] = im.cam_y[p];
    }
    v.plane_cam_valid = im.cam_valid;

    v.unique_tile_count   = ayther_tile_hasher_unique_count(im.tile_hasher.get());
    v.unique_sprite_count = im.sprite_hasher
                              ? ayther_sprite_hasher_unique_count(im.sprite_hasher.get()) : 0u;
    v.unique_audio_count  = ayther_audio_hasher_unique_count(im.audio_hasher.get());

    if (sc) ayther_script_get_shader_params(sc, &v.shader_params);

    v.emu_fps     = im.runner.fps();
    v.tile_ms     = tile_ms;
    v.sprite_ms   = sprite_ms;
    v.audio_ms    = audio_ms;
    v.drc_ratio   = im.audio_enabled ? im.audio.drc_ratio() : 1.0f;
    v.frame_index = im.frame_index;
    v.fps_timing  = timing_fps();
    // R-5 () / : acá vivía el compose por SUPRESIÓN — renders B y C
    // desde un pre-estado serializado, con el original suprimido por los
    // canales 0x102-0x106 del core. Murió con R-5: el primer plano sobre el HD
    // lo da el pase pri-1 de la escena (capa VdpFrente) y el ocultado por
    // elemento viaja en el inventario (SceneElement.hidden). Los gates estaban
    // en `constexpr false` desde entonces, asi que el compilador ya tiraba
    // todo esto; lo que seguia costando era el SHADOW CORE, que se instanciaba
    // igual (copia del DLL + boot) para una maquinaria que no corria.

    // ── R-5 (): publicar la ESCENA por elementos — el insumo del compose
    // sin blit, SIEMPRE (el flag de convivencia se retiró con los canales:
    // criterio de la issue). Al final del produce a propósito: el inventario
    // lee la FrameView ya cableada (celdas, occs, subs, claims). Sin core
    // forkeado los campos quedan vacíos y el renderer cae al blit.
    {
        scene_inventory(im.scene_elements);
        v.scene       = im.scene_elements.data();
        v.scene_count = static_cast<uint32_t>(im.scene_elements.size());
        // OJO orden: VRAM/CRAM se cablean ANTES del cálculo de dirty — los
        // chequeos de las tablas leen v.scene_vram (el bug del bit2 mudo).
        {
            size_t vsz2 = 0, csz2 = 0;
            v.scene_vram = video_ram(&vsz2);  v.scene_vram_size = vsz2;
            v.scene_cram = color_ram(&csz2);  v.scene_cram_size = csz2;
        }
        // Híbrido de R-1: frame con escrituras de efecto visual a mitad de
        // pantalla (señal 0x10E) o con el dim de Animación → el renderer cae
        // al blit; los demás componen.
        // E-3 (): con ABI la señal viene en el snapshot del frame
        // (`fallback_reasons`); sin ABI, del contador legacy 0x10E.
        AYTHER_LEGACY_READ_BEGIN
        const uint32_t raster = im.abi_snap_ok
            ? im.runner.read_raster_fallback_v1(im.abi_snap)
            : im.runner.raster_dirty();
        AYTHER_LEGACY_READ_END
        v.scene_dirty = (raster > 0 ? 1 : 0)
                      | (im.layer_dim_want ? 2 : 0);
        // ABI 1.9 §5.8: `> 0` sigue siendo fallback (arriba), pero dos bits
        // dicen algo más que «el frame se partió» y conviene verlos en el log
        // una vez: OVERFLOW = la multicapa va a devolver RC_JOURNAL_OVERFLOW
        // (fallback, no prefijo); UNSUPPORTED_CONTROLS = un control que este
        // Engine pidió no aplica en este modo (el core lo rechazó).
        if (im.abi_snap_ok) {
            if ((raster & RetroRunner::kRasterReasonJournalOverflow) &&
                !im.raster_overflow_logged) {
                im.raster_overflow_logged = true;
                std::fprintf(stderr,
                    "[AytherSession] journal raster desbordado (>256 eventos en "
                    "un frame): fallback al frame emitido, sin recomposicion\n");
            }
            if ((raster & RetroRunner::kRasterReasonUnsupportedControls) &&
                !im.raster_unsupported_logged) {
                im.raster_unsupported_logged = true;
                std::fprintf(stderr,
                    "[AytherSession] el core rechazo un control de render en "
                    "este modo (UNSUPPORTED_MODE): la sustitucion afectada "
                    "queda apagada en vez de a medias\n");
            }
        }
        // bit2: hscroll por línea/celda CON variación real en el span visible.
        // La tabla se escribe en vblank (la señal 0x10E no la ve) pero las
        // celdas del inventario muestrean H por banda al centro del tile → la
        // composición tendría cizalla sub-tile. Hasta que el pipeline dibuje
        // strips por línea (R-7, parallax), esos frames usan el blit. Con H
        // uniforme (GA toda la demo) se compone normal.
        if (v.scene_vram && v.scene_vram_size >= 0x10000) {
            size_t rsz3 = 0;
            const uint8_t* regs3 = vdp_regs(&rsz3);
            if (regs3 && rsz3 >= 0x20 && (regs3[11] & 3)) {
                const uint32_t hscb  = (uint32_t)(regs3[13] & 0x3F) << 10;
                const uint32_t hmask = (regs3[11] & 3) == 3 ? 0xFF
                                     : (regs3[11] & 3) == 2 ? 0xF8 : 0x07;
                auto entry = [&](uint32_t l) {
                    const uint32_t off = hscb + ((l & hmask) << 2);
                    return (uint32_t)v.scene_vram[off]
                         | ((uint32_t)v.scene_vram[off + 1] << 8)
                         | ((uint32_t)v.scene_vram[off + 2] << 16)
                         | ((uint32_t)v.scene_vram[off + 3] << 24);
                };
                const uint32_t e0 = entry(0);
                for (uint32_t l = 1; l < v.fb_height; ++l)
                    if (entry(l) != e0) { v.scene_dirty |= 4; break; }
            }
            // Variante VERTICAL del mismo límite: vscroll por columna (2-cell)
            // CON variación entre columnas — el inventario da V por celda con
            // la columna del borde izquierdo, el VDP la aplica por columna de
            // píxel → cizalla en los tiles que cruzan el límite (el fondo
            // ondulado de Chemical Plant). Uniforme → compone normal.
            if (regs3 && rsz3 >= 0x20 && (regs3[11] & 4)) {
                const uint8_t* vsr = impl_->vsram_ptr();   // E-5
                const size_t   vsz3 = impl_->runner.vsram_size();
                if (vsr && vsz3 >= 4) {
                    const int cols = (int)((v.fb_width + 15) / 16);
                    uint32_t c0 = (uint32_t)vsr[0] | ((uint32_t)vsr[1] << 8)
                                | ((uint32_t)vsr[2] << 16) | ((uint32_t)vsr[3] << 24);
                    for (int c2 = 1; c2 < cols && (size_t)(c2 * 4 + 3) < vsz3; ++c2) {
                        const uint32_t cv = (uint32_t)vsr[c2 * 4]
                                          | ((uint32_t)vsr[c2 * 4 + 1] << 8)
                                          | ((uint32_t)vsr[c2 * 4 + 2] << 16)
                                          | ((uint32_t)vsr[c2 * 4 + 3] << 24);
                        if (cv != c0) { v.scene_dirty |= 4; break; }
                    }
                }
            }
        }
        size_t rsz2 = 0;
        const uint8_t* regs2 = vdp_regs(&rsz2);
        if (regs2 && rsz2 >= 0x20 && v.scene_cram && v.scene_cram_size >= 0x80) {
            const uint32_t bi2 = (regs2[7] & 0x3F) * 2u;
            v.scene_backdrop = static_cast<uint16_t>(
                v.scene_cram[bi2] | (v.scene_cram[bi2 + 1] << 8));
            v.scene_left_blank = (regs2[0] & 0x20) ? 1 : 0;
        }
    }
    return v;
}

// ---------------------------------------------------------------------------
// Determinism: savestate round-trip
// ---------------------------------------------------------------------------
size_t AytherSession::serialize_size() const { return impl_->runner.serialize_size(); }

Result<void> AytherSession::serialize(std::vector<uint8_t>& out) const {
    return impl_->runner.serialize(out)
             ? Result<void>::ok()
             : Result<void>::fail(ErrorCode::Internal, "core serialize failed");
}

Result<void> AytherSession::unserialize(const std::vector<uint8_t>& in) {
    const bool ok = impl_->runner.unserialize(in);
    if (ok) {
        impl_->rewind.clear();   // timeline branched — drop stale rewind states
        impl_->poke_dirty = false;   // estado limpio restaurado (M5)
        impl_->replay_pos = -1;  // cursor de replay inválido (R7d)
    }
    return ok ? Result<void>::ok()
              : Result<void>::fail(ErrorCode::BadFormat, "core unserialize failed");
}

void AytherSession::reset() {
    Impl& im = *impl_;
    im.runner.reset();
    // E-2 (): las suscripciones NO se serializan y el core las limpia con
    // retro_reset — sin volver a pedirlas, todo lo que se observa por la ABI
    // quedaría mudo desde acá en adelante, y en silencio.
    if (im.ayther_subs_requested && im.runner.has_ayther_v1()) {
        im.ayther_subs_verified = false;
        im.runner.ayther_api()->set_subscriptions(im.ayther_subs_requested);
    }
    im.rewind.clear();           // timeline branched
    im.poke_dirty = false;       // estado limpio (M5)
    im.replay_pos = -1;          // cursor de replay inválido (R7d)
    // : un reset arranca el juego de cero — ninguna instancia live, ni
    // ventana, ni flanco del mundo anterior puede sobrevivirlo (reanudar
    // levantaría un reemplazo anclado a un reloj que ya no existe).
    if (im.audio_enabled) {
        for (const auto& kv : im.audio_live_inst) {
            im.audio.stop_sfx_by_key(kv.first);
            im.audio.stop_event(kv.first);
        }
        for (const auto& w : im.audio_live_seq_win)
            im.audio.stop_sfx_by_key(w.key);
    }
    im.audio_live_inst.clear();
    im.audio_seq_windows.clear();
    im.audio_live_seq_win.clear();
    im.audio_live_seq_next.clear();
    im.audio_live_prev.clear();
    if (im.audio_live_det) ayther_audio_event_reset(im.audio_live_det.get());
    //  F4: mundo nuevo — ni el registro ni lo aprendido siguen valiendo.
    im.live_unmatched.clear();
    im.live_sig_instr.clear();
    im.live_assigned_instr.clear();
    im.live_active.clear();
}

// ---------------------------------------------------------------------------
// Scripting
// ---------------------------------------------------------------------------
Result<void> AytherSession::load_script(const std::string& lua_source, const char* chunk_name) {
    if (!impl_->script)
        return Result<void>::fail(ErrorCode::Internal, "no script env");
    return ayther_script_load_string(impl_->script.get(), lua_source.c_str(), chunk_name)
             ? Result<void>::ok()
             : Result<void>::fail(ErrorCode::BadFormat, "Lua load error");
}

// ---------------------------------------------------------------------------
// Live authoring (Lab) — persistent sprite assignments
// ---------------------------------------------------------------------------
void AytherSession::assign_sprite(uint64_t hash, const std::string& asset_path,
                                  const uint8_t* ref_rgb) {
    if (asset_path.empty()) { unassign_sprite(hash); return; }
    auto& def = impl_->lab_sprite_overrides[hash];
    def.asset = asset_path;
    if (ref_rgb) { for (int c = 0; c < 3; ++c) def.ref_rgb[c] = ref_rgb[c]; }
    else         { def.ref_rgb[0] = def.ref_rgb[1] = def.ref_rgb[2] = 0; }
}

void AytherSession::unassign_sprite(uint64_t hash) {
    impl_->lab_sprite_overrides.erase(hash);
}

void AytherSession::set_pose_preview(const std::vector<PosePreview>& poses) {
    impl_->preview_pose_overrides.clear();
    for (const auto& p : poses)
        if (!p.hashes.empty() && !p.asset.empty())
            impl_->preview_pose_overrides.push_back(p);
    // : acá se instanciaba el shadow core al alimentar poses. Con el
    // compose por supresión muerto desde R-5, esa copia del DLL (9,5 MB a
    // %TEMP%) y ese boot de un emulador entero se pagaban en cada carga de
    // proyecto con poses para una maquinaria que el compilador ya descartaba.
}

void AytherSession::set_tween_preview(const std::vector<TweenPreview>& tweens) {
    if (!impl_->tween) return;
    ayther_tween_clear_overrides(impl_->tween.get());
    for (const auto& t : tweens) {
        if (t.target.empty() || t.frames.empty()) continue;
        std::vector<const char*> fr;
        fr.reserve(t.frames.size());
        for (const auto& f : t.frames) fr.push_back(f.c_str());
        ayther_tween_set_override(impl_->tween.get(),
                                  t.from.empty() ? nullptr : t.from.c_str(),
                                  t.target.c_str(), fr.data(),
                                  static_cast<uint32_t>(fr.size()), t.ticks);
    }
    // Reglas nuevas → los tracks en curso quedan obsoletos.
    ayther_tween_clear(impl_->tween.get());
}

void AytherSession::set_sprite_hidden(const uint64_t* hashes, uint32_t n) {
    impl_->lab_sprite_hidden.assign(hashes, hashes + (hashes ? n : 0));
    impl_->rebuild_hidden_sets();
}

void AytherSession::set_hidden_elements(const HiddenElement* els, uint32_t n) {
    impl_->element_hidden.assign(els, els + (els ? n : 0));
    impl_->rebuild_hidden_sets();
}

void AytherSession::set_element_effects(const ElementEffect* fx, uint32_t n) {
    for (auto& m : impl_->element_fx) m.clear();
    for (uint32_t i = 0; fx && i < n; ++i)
        if (fx[i].layer < 4) impl_->element_fx[fx[i].layer][fx[i].hash] = fx[i];
}

void AytherSession::set_enhanced_elements(const EnhancedElement* els, uint32_t n) {
    for (auto& s : impl_->element_enhance_lab) s.clear();
    for (uint32_t i = 0; els && i < n; ++i)
        if (els[i].layer < 4) impl_->element_enhance_lab[els[i].layer][els[i].hash] = els[i].k;
    impl_->rebuild_enhance_sets();
}


void AytherSession::clear_assignments() {
    impl_->lab_sprite_overrides.clear();
    impl_->lab_tile_overrides.clear();
    impl_->lab_audio_overrides.clear();
    impl_->lab_plane_overrides.clear();
    impl_->mode3.clear_assignments();
    impl_->audio_evt.clear();   // asignaciones por evento (C-A2) + sus ventanas
}

// -- Modo 3 (RAM anchoring) ---------------------------------------------------
Result<void> AytherSession::load_game_profile(const std::string& toml_path) {
    return impl_->mode3.load_profile(toml_path);
}

bool AytherSession::has_game_profile() const noexcept {
    return impl_->mode3.has_profile();
}

void AytherSession::assign_kind(const std::string& kind_name, const std::string& asset_path) {
    impl_->mode3.assign_kind(kind_name, asset_path);
}

// -- Animaciones C-S2 (Componentes): playback HD en fase -----------------------
void AytherSession::define_animation(uint64_t clip_id, const std::string& sheet_asset,
                                     const HdPose* poses, uint32_t pose_count,
                                     int tween_level) {
    impl_->anim.define(clip_id, sheet_asset, poses, pose_count, tween_level);
}

void AytherSession::undefine_animation(uint64_t clip_id) {
    impl_->anim.undefine(clip_id);
}

void AytherSession::clear_animations() { impl_->anim.clear(); }

size_t AytherSession::animation_count() const noexcept { return impl_->anim.clip_count(); }

std::vector<AnimationDef> AytherSession::animation_definitions() const {
    return impl_->anim.definitions();
}

// -- Audios C-A2 (Componentes): sustitución HD por EVENTO ----------------------
size_t AytherSession::resolve_audio_events(const AytherRecording& rec) {
    Impl& im = *impl_;
    im.audio_events_cache.clear();

    AytherBatchEventDetector* det = ayther_audio_evdet_new();
    if (!det) return 0;
    // Un push POR FRAME de la toma (el detector cuenta frames por push): el
    // primer hash de audio del frame, o 0 (silencio) si el frame no tiene
    // batches — mantiene los frames de los eventos alineados a la toma.
    const uint32_t nf  = rec.frame_count();
    const bool     csr = rec.audio_offsets.size() == size_t(nf) + 1;
    for (uint32_t f = 0; f < nf; ++f) {
        uint64_t h = 0;
        if (csr) {
            const uint32_t a = rec.audio_offsets[f], b = rec.audio_offsets[f + 1];
            if (b > a && a < rec.audio_hashes.size()) h = rec.audio_hashes[a];
        }
        ayther_audio_evdet_push(det, h);
    }
    ayther_audio_evdet_flush(det);

    const uint32_t n = ayther_audio_evdet_event_count(det);
    im.audio_events_cache.resize(n);
    if (n) ayther_audio_evdet_get_events(det, im.audio_events_cache.data(), n);
    ayther_audio_evdet_free(det);

    im.audio_evt.resolve(im.audio_events_cache.data(), n);
    if (im.audio_enabled) im.audio.stop_all_events();   // ventanas nuevas: streams viejos fuera
    return n;
}

const AytherAudioEvent* AytherSession::audio_events(size_t* count) const noexcept {
    if (count) *count = impl_->audio_events_cache.size();
    return impl_->audio_events_cache.data();
}

void AytherSession::assign_audio_event(uint64_t signature, const std::string& asset,
                                       bool looping) {
    Impl& im = *impl_;
    im.audio_evt.assign(signature, asset, looping);
    // Re-resolver las ventanas con los eventos ya detectados (si hay toma).
    im.audio_evt.resolve(im.audio_events_cache.data(),
                         static_cast<uint32_t>(im.audio_events_cache.size()));
}

std::vector<AudioEventAssignment> AytherSession::audio_event_assignments() const {
    return impl_->audio_evt.assignments();
}

const AytherAudioEventSub* AytherSession::audio_event_subs(size_t* count) const noexcept {
    if (count) *count = impl_->audio_evt.sub_count();
    return impl_->audio_evt.subs();
}

const char* AytherSession::assignment_for(uint64_t hash) const noexcept {
    auto it = impl_->lab_sprite_overrides.find(hash);
    return it == impl_->lab_sprite_overrides.end() ? "" : it->second.asset.c_str();
}

std::vector<std::pair<uint64_t, std::string>> AytherSession::assignments() const {
    std::vector<std::pair<uint64_t, std::string>> out;
    out.reserve(impl_->lab_sprite_overrides.size());
    for (const auto& [hash, def] : impl_->lab_sprite_overrides)
        out.emplace_back(hash, def.asset);
    // Deterministic order (the map is unordered) so the built pack is stable.
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

std::vector<AytherSession::SpriteAssignment>
AytherSession::sprite_assignments() const {
    std::vector<SpriteAssignment> out;
    out.reserve(impl_->lab_sprite_overrides.size());
    for (const auto& [hash, def] : impl_->lab_sprite_overrides) {
        SpriteAssignment a{hash, def.asset, {def.ref_rgb[0], def.ref_rgb[1],
                                             def.ref_rgb[2]}};
        out.push_back(std::move(a));
    }
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.hash < b.hash; });
    return out;
}

// -- Tiles + audio: same model as sprites above ----------------------------
void AytherSession::assign_tile(uint64_t hash, const std::string& asset_path) {
    if (asset_path.empty()) { unassign_tile(hash); return; }
    impl_->lab_tile_overrides[hash] = asset_path;
}

void AytherSession::unassign_tile(uint64_t hash) {
    impl_->lab_tile_overrides.erase(hash);
}

const char* AytherSession::tile_assignment_for(uint64_t hash) const noexcept {
    auto it = impl_->lab_tile_overrides.find(hash);
    return it == impl_->lab_tile_overrides.end() ? "" : it->second.c_str();
}

std::vector<std::pair<uint64_t, std::string>> AytherSession::tile_assignments() const {
    std::vector<std::pair<uint64_t, std::string>> out;
    out.reserve(impl_->lab_tile_overrides.size());
    for (const auto& [hash, asset] : impl_->lab_tile_overrides)
        out.emplace_back(hash, asset);
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

void AytherSession::assign_audio(uint64_t hash, const std::string& asset_path) {
    if (asset_path.empty()) { unassign_audio(hash); return; }
    impl_->lab_audio_overrides[hash] = asset_path;
}

void AytherSession::unassign_audio(uint64_t hash) {
    impl_->lab_audio_overrides.erase(hash);
}

const char* AytherSession::audio_assignment_for(uint64_t hash) const noexcept {
    auto it = impl_->lab_audio_overrides.find(hash);
    return it == impl_->lab_audio_overrides.end() ? "" : it->second.c_str();
}

std::vector<std::pair<uint64_t, std::string>> AytherSession::audio_assignments() const {
    std::vector<std::pair<uint64_t, std::string>> out;
    out.reserve(impl_->lab_audio_overrides.size());
    for (const auto& [hash, asset] : impl_->lab_audio_overrides)
        out.emplace_back(hash, asset);
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

void AytherSession::assign_plane(uint64_t hash, const std::string& asset_path) {
    if (asset_path.empty()) { unassign_plane(hash); return; }
    impl_->lab_plane_overrides[hash] = asset_path;
}

void AytherSession::unassign_plane(uint64_t hash) {
    impl_->lab_plane_overrides.erase(hash);
}

void AytherSession::define_plane_set(uint64_t id, uint8_t plane, uint16_t w_cells,
                                     uint16_t h_cells, const PlaneSetMember* members,
                                     uint32_t member_count,
                                     const std::string& asset_path,
                                     const uint8_t* ref_rgb) {
    if (!id || !members || member_count == 0 || asset_path.empty()) return;
    Impl::PlaneSetDef d;
    d.plane   = plane;
    d.w_cells = w_cells;
    d.h_cells = h_cells;
    d.asset   = asset_path;
    d.members.assign(members, members + member_count);
    if (ref_rgb) std::memcpy(d.ref_rgb, ref_rgb, 3);
    impl_->plane_sets[id] = std::move(d);
}

void AytherSession::undefine_plane_set(uint64_t id) {
    impl_->plane_sets.erase(id);
}

void AytherSession::clear_plane_sets() { impl_->plane_sets.clear(); }

void AytherSession::define_plane_sequence(uint64_t id,
                                          const PlaneSequenceStep* steps,
                                          uint32_t step_count) {
    if (!id || !steps || step_count == 0) return;
    Impl::PlaneSeqDef d;
    d.steps.reserve(step_count);
    d.assets.reserve(step_count);
    d.durs.reserve(step_count);
    for (uint32_t i = 0; i < step_count; ++i) {
        if (!steps[i].set_id) continue;   // un paso sin Objeto no es un paso
        d.steps.push_back(steps[i].set_id);
        d.assets.emplace_back(steps[i].asset ? steps[i].asset : "");
        d.durs.push_back(steps[i].duration);
        d.total += steps[i].duration ? steps[i].duration : Impl::kSeqDefaultDur;
    }
    // Un solo paso es el Objeto solo, sin nada que ciclar: la sustitución por
    // hash del plane set ya lo cubre y declararlo acá sólo agregaría un reloj
    // que nunca avanza.
    if (d.steps.size() < 2) return;
    impl_->plane_seqs[id] = std::move(d);
    impl_->plane_seq_reindex();
    impl_->seq_clocks.erase(id);   // la definición cambió: el reloj viejo no vale
}

void AytherSession::undefine_plane_sequence(uint64_t id) {
    impl_->plane_seqs.erase(id);
    impl_->seq_clocks.erase(id);
    impl_->plane_seq_reindex();
}

void AytherSession::clear_plane_sequences() {
    impl_->plane_seqs.clear();
    impl_->seq_clocks.clear();
    impl_->set_to_seq.clear();
}

void AytherSession::set_hd_enabled(bool on) noexcept { impl_->hd_enabled = on; }
bool AytherSession::hd_enabled() const noexcept { return impl_->hd_enabled; }

// -- Routing original/HD por subsistema () ------------------------------
void AytherSession::set_subsystem_enabled(Subsystem s, bool on) noexcept {
    const uint32_t bit = subsystem_bit(s);
    if (on) impl_->subsystems_on |=  bit;
    else    impl_->subsystems_on &= ~bit;
}

bool AytherSession::subsystem_enabled(Subsystem s) const noexcept {
    return impl_->sub_on(s);
}

uint32_t AytherSession::subsystems_enabled_mask() const noexcept {
    // Sólo los bits que existen: devolver los 32 haría que un round-trip
    // guardara basura y que un `mask == kAllOn` de mañana no matcheara.
    return impl_->subsystems_on & ((1u << kSubsystemCount) - 1u);
}

void AytherSession::set_subsystems_enabled_mask(uint32_t mask) noexcept {
    impl_->subsystems_on = mask & ((1u << kSubsystemCount) - 1u);
}

// -- Perfiles de remasterización () -------------------------------------
//
// El perfil no guarda estado propio: se APLICA sobre el mismo estado que  y
//  ya manejan, y el activo se DEDUCE comparando. Guardar «el perfil actual»
// aparte sería tener dos verdades sobre lo mismo, y la que se desincroniza es
// siempre la declarativa: alguien apaga un subsistema a mano y el label sigue
// diciendo «Mejorado» sobre algo que ya no lo es.
uint32_t AytherSession::profile_count() const noexcept {
    return impl_->pack ? ayther_pack_profile_count(impl_->pack.get()) : 0;
}

std::string AytherSession::profile_id(uint32_t i) const {
    if (!impl_->pack) return {};
    const char* s = ayther_pack_profile_field(impl_->pack.get(), i, "id");
    return s ? s : "";
}

std::string AytherSession::profile_name(uint32_t i) const {
    if (!impl_->pack) return {};
    const char* s = ayther_pack_profile_field(impl_->pack.get(), i, "name");
    return s ? s : "";
}

bool AytherSession::set_profile(const std::string& id) {
    if (!impl_->pack || id.empty()) return false;
    const int32_t i = ayther_pack_profile_index(impl_->pack.get(), id.c_str());
    // Un perfil que no existe NO se aproxima: aplicar «lo más parecido» dejaría
    // al usuario viendo algo que no pidió sin que nada lo diga.
    if (i < 0) return false;

    set_subsystems_enabled_mask(
        ayther_pack_profile_systems(impl_->pack.get(), (uint32_t)i));
    // Los buses se setean TODOS —los que el perfil silencia y los que no—
    // porque cambiar de perfil tiene que dejar el audio en el estado del perfil
    // nuevo, no en la unión con el anterior.
    const uint32_t muted =
        ayther_pack_profile_muted_buses(impl_->pack.get(), (uint32_t)i);
    for (uint32_t b = 0; b < kAudioBusCount; ++b)
        set_bus_muted(static_cast<AudioBus>(b), (muted & (1u << b)) != 0);
    impl_->profile_hint = id;
    return true;
}

std::string AytherSession::active_profile() const {
    if (!impl_->pack) return {};
    const uint32_t sys = subsystems_enabled_mask();
    uint32_t muted = 0;
    for (uint32_t b = 0; b < kAudioBusCount; ++b)
        if (bus_muted(static_cast<AudioBus>(b))) muted |= (1u << b);

    // La ELECCIÓN del usuario primero, pero sólo si el estado la sostiene: dos
    // perfiles pueden tener el mismo efecto (uno recortado coincide con otro
    // más chico) y ahí deducirlo del estado devolvería cualquiera de los dos.
    // Verificarla en vez de creerle es lo que evita tener dos verdades.
    const uint32_t n = ayther_pack_profile_count(impl_->pack.get());
    auto matches = [&](uint32_t i) {
        return ayther_pack_profile_systems(impl_->pack.get(), i) == sys
            && ayther_pack_profile_muted_buses(impl_->pack.get(), i) == muted;
    };
    if (!impl_->profile_hint.empty()) {
        const int32_t h = ayther_pack_profile_index(impl_->pack.get(),
                                                    impl_->profile_hint.c_str());
        if (h >= 0 && matches((uint32_t)h)) return impl_->profile_hint;
    }
    for (uint32_t i = 0; i < n; ++i) {
        if (!matches(i)) continue;
        const char* id = ayther_pack_profile_field(impl_->pack.get(), i, "id");
        return id ? id : "";
    }
    // Vacío es un resultado legítimo: el usuario tocó algo y el estado dejó de
    // ser el que cualquier perfil describe. Ése es el «custom» del alcance de
    // la issue — no se declara, se alcanza.
    return {};
}

bool AytherSession::apply_default_profile() {
    if (!impl_->pack) return false;
    const uint32_t i = ayther_pack_default_profile(impl_->pack.get());
    const char* id = ayther_pack_profile_field(impl_->pack.get(), i, "id");
    return id && set_profile(id);
}

// -- Validación de packs () ---------------------------------------------
std::vector<AytherSession::PackFinding>
AytherSession::validate_pack(const std::string& pack_path) const {
    std::vector<PackFinding> out;
    if (pack_path.empty()) return out;

    // El contexto sale de la sesión: la plataforma (Sega CD vs cartucho la sabe
    // el runner) y el build_id del core cargado. La ROM NO se declara acá — el
    // Engine no calcula su CRC32, y decir «no se sabe» hace que el informe lo
    // reporte como no verificado en vez de darlo por bueno. El frontend que sí
    // lo tenga (el Lab lo tiene: el game_id del proyecto es "crc32:…") puede
    // llamar al FFI directamente con ese dato.
    const std::string plat = impl_->runner.cd_media() ? "segacd" : "megadrive";
    // El build_id del core NO viene NUL-terminado (es un span de la ABI), así
    // que se copia: pasar el puntero crudo leería de más.
    std::string build_id;
    if (const ayther_interface_v1* api = impl_->runner.ayther_api())
        if (api->build_id && api->build_id_size)
            build_id.assign(api->build_id, api->build_id_size);

    AytherValidateCtx ctx{};
    ctx.rom_crc32      = 0;
    ctx.has_rom        = false;
    ctx.platform       = plat.c_str();
    ctx.core_build_id  = build_id.empty() ? nullptr : build_id.c_str();
    ctx.engine_version = nullptr;
#ifdef NDEBUG
    ctx.release_build  = true;
#else
    ctx.release_build  = false;
#endif

    AytherPackReport* rep = ayther_pack_validate(pack_path.c_str(), &ctx);
    if (!rep) return out;
    const uint32_t n = ayther_pack_report_count(rep);
    out.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        const char* code = ayther_pack_report_code(rep, i);
        const char* msg  = ayther_pack_report_message(rep, i);
        out.push_back(PackFinding{
            ayther_pack_report_severity(rep, i) == 0,
            code ? code : "", msg ? msg : ""});
    }
    ayther_pack_report_free(rep);
    return out;
}

// -- Buses lógicos de audio () ------------------------------------------
void AytherSession::set_bus_volume(AudioBus bus, float gain) noexcept {
    const uint32_t i = static_cast<uint32_t>(bus);
    if (i >= kAudioBusCount) return;
    // El tope es el mismo que el de la ganancia autorada de una Secuencia
    // (0..2): dos escalas distintas para lo mismo confundirían al que mira los
    // dos números juntos en la UI.
    const float g = gain < 0.0f ? 0.0f : (gain > 2.0f ? 2.0f : gain);
    impl_->bus_gain[i] = g;

    // Y ALCANZA A LO QUE YA ESTÁ SONANDO. Sin esto, la ganancia sólo se
    // aplicaría al crear el stream: bajar el bus de Música con la música
    // sonando no se oiría hasta el próximo disparo — que en un loop musical
    // largo es NUNCA, y arrastrar un slider que no hace nada es justo lo que
    // hace pensar que el control está roto. `set_sfx_gain_by_key` existe para
    // esto (misma lección que el volumen por Secuencia).
    if (!impl_->audio_enabled) return;
    for (const auto& sq : impl_->audio_seq_subs)
        if (sq.bus == bus)
            impl_->audio.set_sfx_gain_by_key(sq.key, sq.gain * g);
    // Las asignaciones sueltas no tienen ganancia autorada propia: su volumen
    // ES el del bus (Efectos, por defecto).
    for (const auto& [sig, asset] : impl_->audio_event_assign) {
        (void)asset;
        if (impl_->bus_of_signature(sig) != bus) continue;
        impl_->audio.set_sfx_gain_by_key(sig, g);
    }
}

float AytherSession::bus_volume(AudioBus bus) const noexcept {
    const uint32_t i = static_cast<uint32_t>(bus);
    return i < kAudioBusCount ? impl_->bus_gain[i] : 1.0f;
}

void AytherSession::set_bus_muted(AudioBus bus, bool muted) noexcept {
    const uint32_t i = static_cast<uint32_t>(bus);
    if (i >= kAudioBusCount) return;
    impl_->bus_mute[i] = muted;
    // Silenciar A MITAD tiene que callar lo que ya está sonando, no sólo evitar
    // el próximo disparo: un asset de Secuencia dura segundos y desde afuera se
    // vería igual que «el mute no funciona» (la lección de , que costó un
    // reporte).
    if (muted && impl_->audio_enabled) {
        for (const auto& sq : impl_->audio_seq_subs)
            if (sq.bus == bus) {
                impl_->audio.stop_sfx_by_key(sq.key);
                impl_->audio.stop_event(sq.key);
            }
        // Las asignaciones sueltas viven en el bus de Efectos; sin Secuencia no
        // hay lista que recorrer, así que se cortan por firma asignada.
        if (bus == AudioBus::Sfx)
            for (const auto& [sig, asset] : impl_->audio_event_assign) {
                (void)asset;
                if (impl_->bus_of_signature(sig) != AudioBus::Sfx) continue;
                impl_->audio.stop_sfx_by_key(sig);
                impl_->audio.stop_event(sig);
            }
    }
}

bool AytherSession::bus_muted(AudioBus bus) const noexcept {
    const uint32_t i = static_cast<uint32_t>(bus);
    return i < kAudioBusCount ? impl_->bus_mute[i] : false;
}

// -- Degradación segura () ----------------------------------------------
uint32_t AytherSession::auto_disabled_subsystems() const noexcept {
    return impl_->auto_disabled_on;
}

std::string AytherSession::degradation_message() const {
    const Impl& im = *impl_;
    if (!im.auto_disabled_on) return {};
    std::string what;
    if (im.auto_disabled_on & subsystem_bit(Subsystem::Music)) what = "La música HD";
    if (im.auto_disabled_on & subsystem_bit(Subsystem::Sfx))
        what = what.empty() ? "Los efectos HD" : "El audio HD";
    std::string m = what + " se apagó: " + std::to_string(im.escalation.total()) +
                    " archivos del pack no se pudieron reproducir. "
                    "Se sigue oyendo el juego original.";
    // El PACK, porque con los assets nombrados por hash es lo único que permite
    // volver al proyecto que lo horneó.
    if (!im.pack_path.empty()) {
        const size_t slash = im.pack_path.find_last_of("/\\");
        m += "  (pack: " + (slash == std::string::npos ? im.pack_path
                                                       : im.pack_path.substr(slash + 1)) + ")";
    }
    return m;
}

void AytherSession::clear_auto_disabled() noexcept {
    Impl& im = *impl_;
    im.subsystems_on |= im.auto_disabled_on;
    im.auto_disabled_on = 0;
    im.escalation.clear();
    // También los fallos por key: si el usuario pide reintentar, es porque
    // arregló algo — y dejar la lista vieja haría que el reintento no cambiara
    // nada y pareciera que el botón no funciona.
    im.hd_failed_keys.clear();
}

SubsystemAvailability
AytherSession::subsystem_availability(Subsystem s) const noexcept {
    // Sin pack no hay nada que declarar: la respuesta es «no se sabe», que es
    // la misma que da un pack legacy. Decir «no lo trae» sería afirmar algo que
    // nadie midió, y el frontend lo mostraría como un subsistema ausente.
    if (!impl_->pack) return SubsystemAvailability::Unknown;
    if (!ayther_pack_declares_systems(impl_->pack.get()))
        return SubsystemAvailability::Unknown;
    const uint32_t m = ayther_pack_systems(impl_->pack.get());
    return (m & subsystem_bit(s)) ? SubsystemAvailability::Present
                                  : SubsystemAvailability::Absent;
}

void AytherSession::define_screen(uint64_t id, uint8_t plane_mask,
                                  const ScreenCell* cells, uint32_t cell_count,
                                  float min_match, float max_extra,
                                  const std::string& asset_path) {
    // El asset NO es requisito para DEFINIR: un Cuadro sin asset se reconoce
    // igual y sólo no dibuja. Exigirlo rompía el caso de una Cinemática de N
    // Cuadros cubierta por un solo video —ningún paso se podía dar de alta— y
    // también impedía que el Lab mostrara «esta pantalla ya está reconocida,
    // falta asignarle algo».
    if (!id || !cells || cell_count == 0) return;
    Impl::ScreenDef d;
    d.mask      = plane_mask ? plane_mask : 0x07;
    d.min_match = min_match > 0.0f ? min_match : 0.92f;
    d.max_extra = max_extra >= 0.0f ? max_extra : 0.08f;
    d.asset     = asset_path;
    d.cells.reserve(cell_count);
    for (uint32_t i = 0; i < cell_count; ++i) {
        const ScreenCell& c = cells[i];
        if (c.plane > 2 || !(d.mask & (1u << c.plane))) continue;
        const uint32_t k = ((uint32_t)c.plane << 24) | ((uint32_t)c.col << 8) | c.row;
        d.cells.emplace(k, c.hash);
        // Firma POR CAPA, con el MISMO término que produce_frame — el
        // reconocimiento compara igualdad exacta contra estos valores.
        uint64_t t = c.hash;
        t ^= (uint64_t)c.col * 0x9E3779B97F4A7C15ull;
        t ^= (uint64_t)c.row * 0xC2B2AE3D27D4EB4Full;
        t ^= (uint64_t)(c.plane + 1) * 0x165667B19E3779F9ull;
        t ^= t >> 33; t *= 0xFF51AFD7ED558CCDull; t ^= t >> 29;
        d.sig_plane[c.plane] += t;
        ++d.cells_plane[c.plane];
        d.hashes_plane[c.plane].insert(c.hash);
    }
    if (d.cells.empty()) return;
    impl_->screens[id] = std::move(d);
}

void AytherSession::undefine_screen(uint64_t id) {
    impl_->screens.erase(id);
    if (impl_->screen_active == id) impl_->screen_active = 0;
    if (impl_->screen_cand   == id) { impl_->screen_cand = 0; impl_->screen_streak = 0; }
}

// ---------------------------------------------------------------------------
// VIDEO del paso () — resolver qué frame del clip toca en este frame de juego.
//
// LA REGLA: aritmética ABSOLUTA sobre el ancla, nunca un acumulador.
//
//     video_frame = (frame_index − anchor) mod frame_count
//
// Un acumulador («+1 por frame») se desincroniza con el primer re-produce y no
// se recupera. La resta absoluta da el resultado correcto aunque el motor
// produzca el mismo frame diez veces o salte cincuenta de golpe.
// ---------------------------------------------------------------------------
void AytherSession::Impl::video_tick(const std::string& path) {
    screen_sub_n = 0;   // el video no emite quad; ver el enrutado en el pick

    // Lazy-open, por STREAMING donde se pueda (). Tres caminos, en orden de
    // preferencia:
    //
    //   1. Pack con la entrada direccionable por rango → fuente sobre el pack.
    //      RAM = un paquete. Es el camino normal desde .
    //   2. Pack sin streaming (pack legacy, o entrada deflateada) → lectura
    //      entera, como antes. Se conserva porque un pack viejo tiene que
    //      seguir reproduciéndose.
    //   3. DISCO: en el LAB se autora contra el proyecto, sin pack, y el asset
    //      del paso es una ruta absoluta al .ivf. Sin esto el video sólo se veía
    //      en el player y en autoría no se veía NUNCA — el mismo modo de fallo
    //      que ya tuvieron los assets HD antes de leer del proyecto (2026-08-07).
    auto it = videos.find(path);
    if (it == videos.end()) {
        auto clip = std::make_unique<ayther::VideoClip>();
        bool ok = false;
        std::string err;

        if (auto psrc = ayther::video_source_from_pack(pack.get(), path)) {
            // : el ÍNDICE HORNEADO, si el pack lo trae. Sin él hay que
            // barrer el .ivf entero para saber dónde está cada frame, y eso
            // cuesta lo mismo que leerlo completo. Se lee por `read` y no por
            // rango a propósito: son 12 bytes por frame (21 KB en un clip de
            // 30 s) y de una sola lectura verificada.
            //
            // Que no esté es el caso NORMAL en un pack anterior a : no se
            // loguea nada y se cae al barrido.
            std::vector<uint8_t> idx;
            if (pack) {
                const std::string ip = ayther::video_index_path(path);
                const int64_t isz = ayther_pack_file_size(pack.get(), ip.c_str());
                if (isz > 0) {
                    idx.resize((size_t)isz);
                    if (ayther_pack_read(pack.get(), ip.c_str(), idx.data(),
                                         idx.size()) <= 0)
                        idx.clear();
                }
            }
            ok = clip->open(std::move(psrc), idx.empty() ? nullptr : idx.data(),
                            idx.size(), &err);
        }
        if (!ok) {
            std::vector<uint8_t> buf;
            if (pack) {
                const int64_t sz = ayther_pack_file_size(pack.get(), path.c_str());
                if (sz > 0) {
                    buf.resize((size_t)sz);
                    if (ayther_pack_read(pack.get(), path.c_str(), buf.data(),
                                         buf.size()) <= 0)
                        buf.clear();
                }
            }
            if (!buf.empty()) {
                ok = clip->open(buf.data(), buf.size(), &err);
            } else if (auto fsrc = ayther::video_source_from_file(path)) {
                ok = clip->open(std::move(fsrc), &err);
            }
        }
        if (!ok && !err.empty())
        {
            // El BUILD ID va en la línea del error y no sólo en la del open
            // (/). Un usuario que reporta un problema pega UNA línea, no
            // el log entero: sin el build acá, el hash del asset no se puede
            // resolver contra ningún log de horneado.
            const char* bid = pack ? ayther_pack_build_id(pack.get()) : nullptr;
            if (bid && *bid)
                std::fprintf(stderr, "[video] %s - %s build %s: %s\n",
                             path.c_str(), ayther_pack_game_id(pack.get()),
                             bid, err.c_str());
            else
                std::fprintf(stderr, "[video] %s: %s\n", path.c_str(), err.c_str());
        }
        // Se cachea AUNQUE haya fallado: sin esto se reintentaría abrir (y
        // loguear) el mismo clip roto en cada frame — el patrón de negative
        // cache que VkSprite ya usa para los assets ausentes.
        if (!ok) clip.reset();
        it = videos.emplace(path, std::move(clip)).first;
    }
    ayther::VideoClip* clip = it->second.get();
    if (!clip || !clip->frame_count()) { vid_out = {}; vid_on = false; return; }

    // Re-ancla SÓLO al cambiar de paso. Si el paso sigue siendo el mismo, el
    // ancla sobrevive a `kinematic_reset()` — que es lo que impide que un hipo
    // de playback rebobine el video (ver la nota de VideoPhase).
    if (kine_active != vid.kin || kine_step != vid.step || vid.anchor < 0) {
        vid.kin = kine_active;
        vid.step = kine_step;
        vid.anchor = (int64_t)frame_index;
    }

    // Offset del paso: en qué frame del clip arranca ESTE plano. Es lo que hace
    // que un solo video cubra varios pasos y que entrar por el medio de la
    // secuencia caiga donde corresponde — la posición sale del CONTENIDO (qué
    // paso matcheó) más el offset, nunca de un contador.
    uint32_t off = 0;
    bool     loop = false;
    if (auto kt = kinematics.find(kine_active); kt != kinematics.end()) {
        if (kine_step < kt->second.video_offsets.size())
            off = kt->second.video_offsets[kine_step];
        loop = kt->second.loop;
    }

    // int64 con piso en 0: un seek HACIA ATRÁS dentro del mismo paso da delta
    // negativo, y en uint32 eso subdesborda a ~4 mil millones. Es el defecto
    // documentado de AnimationPlayer (ayther_animation.cpp), que acá no se
    // repite.
    const int64_t d = (int64_t)frame_index - vid.anchor;
    // `off` y `d` están en frames de JUEGO; el clip tiene los SUYOS. Sin esta
    // razón el reproductor asumía 1:1 y un clip a 30 sobre una toma a 59,92
    // salía al DOBLE de velocidad y duraba la mitad (2026-08-07). El clip sin
    // time base declarado cae a 1:1, que es el comportamiento viejo.
    const double vfps = clip->fps();
    const double gfps = runner.fps();
    const double rate = (vfps > 0.1 && gfps > 1.0) ? vfps / gfps : 1.0;
    const int64_t gframes = (int64_t)off + (d < 0 ? 0 : d);
    const int64_t vframes = (int64_t)(double(gframes) * rate + 0.5);
    // Fuera del clip: CICLAR o SOSTENER, según lo pidió el autor (Loop). El
    // default es sostener porque rebobinar una escena narrada a mitad de camino
    // es un defecto, no una función — y un video que se queda corto tiene que
    // notarse para que se re-encode. Con Loop en true (un fondo de lluvia, un
    // fuego) el ciclo es justamente lo que se busca.
    const uint32_t last = clip->frame_count() - 1;
    const int64_t  n    = (int64_t)clip->frame_count();
    const uint32_t idx  =
        vframes < 0        ? 0u
      : loop               ? (uint32_t)(vframes % n)
      : vframes > (int64_t)last ? last
                           : (uint32_t)vframes;

    if (const char* dbg = std::getenv("AYTHER_VIDEO_DEBUG"); dbg && *dbg == '1')
        std::fprintf(stderr,
                     "[video] f=%llu kin=%llx step=%u off=%u anchor=%lld d=%lld "
                     "rate=%.3f idx=%u/%u\n",
                     (unsigned long long)frame_index,
                     (unsigned long long)kine_active, kine_step, off,
                     (long long)vid.anchor, (long long)d, rate, idx,
                     clip->frame_count());

    if (const ayther::VideoFrameView* f = clip->decode(idx)) {
        vid_out = *f;
        vid_on  = true;
    } else {
        vid_out = {};
        vid_on  = false;
    }

    // ── AUDIO de la Cinemática ───────────────────────────────────────────────
    // Misma aritmética de ancla que la imagen, distinta cadencia de aplicación:
    // el video se re-indexa por frame (decodificar un keyframe es barato y no
    // se oye) pero el audio NO se puede reiniciar por paso — serían 44 cortes
    // en cuatro segundos. Se arranca una vez y se re-sincroniza SÓLO cuando el
    // ancla salta más que la tolerancia, que es la firma de un scrub.
    std::string aud;
    float       again = 1.0f, ggain = 1.0f;
    if (auto kt = kinematics.find(kine_active); kt != kinematics.end()) {
        aud   = kt->second.audio;
        again = kt->second.gain;
        ggain = kt->second.game_gain;
    }
    // El ducking de la BANDA SONORA no depende de que la Cinemática traiga
    // pista propia: bajar el juego para que se lea un texto en pantalla es un
    // uso legítimo por sí solo. Se aplica mientras el video corre y lo devuelve
    // video_audio_stop().
    if (audio.game_gain() != ggain)
        if (const char* dbg = std::getenv("AYTHER_VIDEO_DEBUG"); dbg && *dbg == '1')
            std::fprintf(stderr, "[video/audio] banda sonora %.0f%% -> %.0f%%\n",
                         audio.game_gain() * 100.0, ggain * 100.0);
    audio.set_game_gain(ggain);
    if (aud.empty()) {
        // Sin pista: sólo se corta el stream, el ducking SIGUE (lo pidió la
        // Cinemática y sigue activa). Por eso no se llama a video_audio_stop().
        if (vaud.on) { audio.stop_sfx_by_key(kVideoAudioKey); vaud.on = false; }
        return;
    }

    // Pausa / re-produce del MISMO frame: la toma no avanza. Se corta a los
    // pocos ticks y no al primero, porque produce_frame no es 1:1 con los
    // frames emulados (compose y replay_invalidate re-producen) y cortar en el
    // primer repetido picaría el audio en pleno playback.
    constexpr int kStillTicks = 3;
    if ((int64_t)frame_index == vaud.last_f) {
        if (vaud.on && ++vaud.still >= kStillTicks) video_audio_stop();
        return;
    }
    // ¿SALTÓ la toma? Se mira el frame, no el offset del paso. Parece más
    // indirecto y es al revés: el offset del paso sale de dónde se CAPTURÓ el
    // Cuadro y el paso entra donde el contenido MATCHEA, y esos dos números no
    // tienen por qué coincidir — el último Cuadro de una escena que se queda
    // quieta matchea decenas de frames antes de su captura. Anclar contra el
    // offset daba 53 resincronizaciones en cuatro segundos (medido), o sea un
    // tartamudeo continuo. El frame de la toma, en cambio, avanza de a uno
    // reproduciendo y salta sólo cuando el usuario scrubbea, que es exactamente
    // cuando hay que re-sincronizar.
    constexpr int64_t kJumpFrames = 4;
    const int64_t df = (int64_t)frame_index - vaud.last_f;
    const bool jumped = vaud.last_f < 0 || df < 0 || df > kJumpFrames;
    vaud.last_f = (int64_t)frame_index;
    vaud.still  = 0;

    const bool resync = !vaud.on || vaud.kin != kine_active || jumped;
    if (resync && gfps > 1.0) {
        if (vaud.on) audio.stop_sfx_by_key(kVideoAudioKey);
        audio.play_oneshot_asset_file(aud, kVideoAudioKey,
                                      double(off) / gfps, again);
        if (const char* dbg = std::getenv("AYTHER_VIDEO_DEBUG"); dbg && *dbg == '1')
            std::fprintf(stderr, "[video/audio] resync f=%llu t=%.3fs %s\n",
                         (unsigned long long)frame_index, double(off) / gfps,
                         aud.c_str());
        vaud.kin    = kine_active;
        vaud.anchor = (int64_t)frame_index - (int64_t)off;   // informativo
        vaud.gain   = again;
        vaud.on     = true;
    } else if (vaud.on && again != vaud.gain) {
        // Slider movido con la escena corriendo: se ajusta el stream VIVO. Un
        // resync acá volvería a cuear el audio desde el offset y se oiría un
        // salto por cada píxel de arrastre.
        audio.set_sfx_gain_by_key(kVideoAudioKey, again);
        vaud.gain = again;
    }
}

void AytherSession::define_kinematic(uint64_t id, const KinematicStep* steps,
                                     uint32_t step_count, uint32_t gap_frames,
                                     const KinematicMedia* media) {
    if (!id || !steps || step_count == 0) return;
    Impl::KinematicDef d;
    d.gap = gap_frames;
    if (media) {
        d.loop      = media->loop;
        d.audio     = media->audio ? media->audio : "";
        d.gain      = media->gain;
        d.game_gain = media->game_gain;
    }
    d.steps.reserve(step_count);
    d.assets.reserve(step_count);
    d.video_offsets.reserve(step_count);
    for (uint32_t i = 0; i < step_count; ++i) {
        if (!steps[i].screen_id) continue;   // un paso sin Cuadro no es un paso
        d.steps.push_back(steps[i].screen_id);
        d.assets.emplace_back(steps[i].asset ? steps[i].asset : "");
        d.video_offsets.push_back(steps[i].video_offset);
    }
    // Una secuencia de un solo paso es un Cuadro con otro nombre: no aporta
    // orden ni cancelación, y ocuparía el rango superior sin motivo.
    if (d.steps.size() < 2) return;
    impl_->kinematics[id] = std::move(d);
    impl_->kinematic_reindex();
    impl_->kinematic_reset();   // la definición cambió: el cursor viejo no vale
}

void AytherSession::undefine_kinematic(uint64_t id) {
    impl_->kinematics.erase(id);
    impl_->kinematic_reindex();
    if (impl_->kine_active == id) impl_->kinematic_reset();
}

void AytherSession::clear_kinematics() {
    impl_->kinematics.clear();
    impl_->screen_to_kin.clear();
    impl_->kinematic_reset();
}

void AytherSession::clear_screens() {
    impl_->screens.clear();
    impl_->screen_active = impl_->screen_cand = 0;
    impl_->screen_streak = 0;
    impl_->screen_sub_n  = 0;
    impl_->screen_presence_n = 0;
}

void AytherSession::define_panorama(uint64_t id, uint8_t plane,
                                    int32_t origin_x, int32_t origin_y,
                                    uint16_t w_cells, uint16_t h_cells,
                                    const PanoramaCell* cells, uint32_t cell_count,
                                    const std::string& asset_path) {
    if (!id || !cells || cell_count == 0) return;
    Impl::PanoramaDef d = Impl::build_panorama(plane, origin_x, origin_y,
                                               w_cells, h_cells,
                                               cells, cell_count, asset_path);
    // Referencia de luma para el FUNDIDO del quad: la CRAM del momento de
    // definir (recién barrida la tira, escena a niveles normales). Si está
    // apagada (proyecto recién abierto, sin frame), queda el fallback.
    {
        const uint8_t* cram = impl_->cram_ptr();           // E-5
        const size_t   csz  = impl_->runner.color_ram_size();
        if (cram && csz >= 4 * 16 * 2) {
            double luma = 0.0;
            for (int p = 0; p < 4; ++p) {
                double r = 0, g = 0, b = 0;
                for (int i = 1; i < 16; ++i) {
                    const size_t ce = (size_t)p * 32 + (size_t)i * 2;
                    const uint16_t v = (uint16_t)(cram[ce] | (cram[ce + 1] << 8));
                    r += v & 7; g += (v >> 3) & 7; b += (v >> 6) & 7;
                }
                const double rn = r / 105.0, gn = g / 105.0, bn = b / 105.0;
                luma += 0.299 * rn + 0.587 * gn + 0.114 * bn;
            }
            luma /= 4.0;
            if (luma >= 0.05) d.ref_luma = luma;
        }
        // La referencia CROMÁTICA (ref_ch/ref_w) NO se captura acá: este
        // camino corre en cada sync del catálogo con la CRAM de la pantalla
        // que esté viva, que puede no ser la de esta tira. Se captura ANCLADA
        // en produce_frame (peak-hold) — ver PanoramaDef::ref_peak. El
        // ref_luma de arriba queda como valor inicial pre-anclaje.
    }
    impl_->panoramas[id] = std::move(d);
}

AytherSession::Impl::PanoramaDef
AytherSession::Impl::build_panorama(uint8_t plane, int32_t ox, int32_t oy,
                                    uint16_t w, uint16_t h,
                                    const AytherSession::PanoramaCell* cells,
                                    uint32_t n, const std::string& asset) {
    PanoramaDef d;
    d.plane    = plane > 2 ? 0 : plane;
    d.origin_x = ox; d.origin_y = oy;
    d.w_cells  = w;  d.h_cells  = h;
    d.asset    = asset;
    d.total_cells = n;
    // Rareza: se cuenta cuántas veces aparece cada hash en la tira y sólo los
    // POCO frecuentes entran al índice de anclaje. Un tile de cielo repetido
    // 500 veces no dice dónde está la cámara y multiplica el costo del voto.
    std::unordered_map<uint64_t, uint32_t> freq;
    freq.reserve(n);
    for (uint32_t i = 0; i < n; ++i) ++freq[cells[i].hash];
    d.by_pos.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        const AytherSession::PanoramaCell& c = cells[i];
        d.by_pos[pano_key(c.lx, c.ly)].push_back(c.hash);   // la tira ENTERA
        if (freq[c.hash] > kPanoramaRare) continue;
        d.anchors[c.hash].emplace_back(c.lx, c.ly);   // sólo los raros (anclaje)
        ++d.rare_cells;
    }
    //  EM-8.1: qué tan LIMPIA es la tira. Una posición con más de un hash
    // distinto es ambigua: puede ser una celda animada (legítimo) o dos tramos
    // del nivel apilados por un barrido que cruzó de zona (contaminación).
    //
    // Importa porque `pano_pos_matches` acepta CUALQUIERA de los hashes de la
    // posición: con una tira muy ambigua la verificación de cobertura matchea
    // todo y el anclaje se declara válido contra un hash que el PNG no dibuja.
    // Medido en Sonic 3 & K: cobertura 100 % y la lámina mostrando OTRA zona.
    //
    // En el área nativa eso queda tapado —las celdas vivas que la tira no
    // reclamó se dibujan encima— pero el área extendida no tiene con qué
    // corregirse, así que este número es el que decide si se puede extender.
    {
        uint32_t ambiguous_positions = 0;
        for (const auto& [k, hs] : d.by_pos) {
            (void)k;
            bool has_multiple_hashes = false;
            for (size_t i = 1; i < hs.size() && !has_multiple_hashes; ++i)
                if (hs[i] != hs[0]) has_multiple_hashes = true;
            if (has_multiple_hashes) ++ambiguous_positions;
        }
        const size_t pos = d.by_pos.size();
        d.clean_pct = pos ? (uint32_t)((pos - ambiguous_positions) * 100 / pos) : 0u;
    }
    return d;
}

void AytherSession::undefine_panorama(uint64_t id) {
    impl_->panoramas.erase(id);
    if (impl_->pano_id == id) { impl_->pano_id = 0; impl_->pano_valid = false; }
}

void AytherSession::clear_panoramas() {
    impl_->panoramas.clear();
    impl_->pano_id = 0; impl_->pano_valid = false;
    impl_->pano_votes = impl_->pano_cells = 0;
}

const char* AytherSession::plane_assignment_for(uint64_t hash) const noexcept {
    auto it = impl_->lab_plane_overrides.find(hash);
    return it == impl_->lab_plane_overrides.end() ? "" : it->second.c_str();
}

std::vector<std::pair<uint64_t, std::string>> AytherSession::plane_assignments() const {
    std::vector<std::pair<uint64_t, std::string>> out;
    out.reserve(impl_->lab_plane_overrides.size());
    for (const auto& [hash, asset] : impl_->lab_plane_overrides)
        out.emplace_back(hash, asset);
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

// ---------------------------------------------------------------------------
// Animation clips (C-S1) — detected looping cycles (ordered poses + timing)
// ---------------------------------------------------------------------------
size_t AytherSession::animation_clip_count() const noexcept {
    return impl_->sprite_hasher
        ? ayther_sprite_hasher_clip_count(impl_->sprite_hasher.get()) : 0;
}

void AytherSession::reset_animation_detection() noexcept {
    if (impl_->sprite_hasher)
        ayther_sprite_hasher_reset_animation_grouper(impl_->sprite_hasher.get());
}

std::vector<AytherSession::AnimationClip> AytherSession::animation_clips() const {
    std::vector<AnimationClip> out;
    if (!impl_->sprite_hasher) return out;
    const uint32_t n = ayther_sprite_hasher_clip_count(impl_->sprite_hasher.get());
    out.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        uint64_t id = 0; uint8_t looping = 0;
        AytherAnimFrame frames[64] = {};
        const uint32_t fc = ayther_sprite_hasher_get_clip(
            impl_->sprite_hasher.get(), i, &id, &looping, frames, 64);
        if (fc == 0xFFFFFFFFu) continue;   // out of range (shouldn't happen)
        AnimationClip c;
        c.id = id; c.looping = looping != 0;
        const uint32_t kept = fc < 64 ? fc : 64;
        c.frames.reserve(kept);
        for (uint32_t k = 0; k < kept; ++k)
            c.frames.push_back({ frames[k].pose, frames[k].duration });
        out.push_back(std::move(c));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Rewind + fast-forward (R6)
// ---------------------------------------------------------------------------
void AytherSession::enable_rewind(bool on, int seconds) {
    if (on) {
        const size_t state_size = impl_->runner.serialize_size();
        const double fps        = impl_->runner.fps() > 1.0 ? impl_->runner.fps() : 60.0;
        const uint32_t cap      = static_cast<uint32_t>(fps * (seconds > 0 ? seconds : 10));
        impl_->rewind.configure(state_size, cap);
        impl_->rewind.set_enabled(state_size > 0);
        std::fprintf(stdout, "[AytherSession] Rewind on: %u frames (~%ds), state=%zu B\n",
                     cap, seconds, state_size);
    } else {
        impl_->rewind.set_enabled(false);
    }
}

const FrameView* AytherSession::rewind_step() {
    Impl& im = *impl_;
    if (!im.rewind.enabled()) return nullptr;
    // pop() drops the current state and yields the previous one into scratch.
    if (!im.rewind.pop(im.rewind_scratch)) return nullptr;
    if (!im.runner.unserialize(im.rewind_scratch)) return nullptr;
    if (im.frame_index > 0) --im.frame_index;
    im.replay_pos = -1;        // cursor de replay inválido (R7d)
    return &produce_frame();   // re-render the restored frame
}

bool   AytherSession::rewind_enabled()      const noexcept { return impl_->rewind.enabled(); }
size_t AytherSession::rewind_frames()       const noexcept { return impl_->rewind.frames(); }
size_t AytherSession::rewind_memory_bytes() const noexcept { return impl_->rewind.memory_bytes(); }

void  AytherSession::set_speed(float mult) noexcept {
    impl_->speed = mult < 0.1f ? 0.1f : (mult > 16.0f ? 16.0f : mult);
}
float AytherSession::speed() const noexcept { return impl_->speed; }

void AytherSession::set_audio_muted(bool m) noexcept { impl_->audio.set_muted(m); }
bool AytherSession::audio_muted() const noexcept { return impl_->audio.is_muted(); }
void AytherSession::set_audio_mute_hashes(const uint64_t* hashes, uint32_t n) noexcept {
    impl_->audio.set_user_mute_hashes(hashes, n);
}

void AytherSession::stop_audio_preview() {
    impl_->audio.stop_oneshot();
    // También el preview HD de la lane de Secuencia (preview_asset_file → SFX
    // con key "SEQ0", no el stream de preview): pausar el play debe CALLAR el
    // audio, no solo frenar el cabezal (reporte 2026-07-23). Fade rápido.
    impl_->audio.stop_sfx_by_key(0x53455130ull /* "SEQ0" */);
    // Y el corte INMEDIATO de todo one-shot `preview` (reporte 2026-08-22: el
    // ▶ de la Biblioteca de Audios seguía sonando tras cerrar el diálogo).
    // El fade de arriba depende de tick(), que solo corre en el flush audible
    // del produce — en PAUSA (el estado típico con un diálogo abierto) nunca
    // llega, y el preview drenaba entero.
    impl_->audio.stop_preview_sfx();
}

bool AytherSession::audio_preview_playing() const { return impl_->audio.preview_playing(); }

size_t AytherSession::preview_audio(const AytherRecording& rec, uint64_t hash) {
    if (hash == 0 || rec.empty()) return 0;
    if (rec.audio_offsets.size() != rec.frame_count() + 1) return 0;   // necesita .arp v7

    // Primer frame donde suena ese hash (sólo localización — la captura es por frame).
    uint32_t f = UINT32_MAX;
    for (uint32_t i = 0; i + 1 < rec.audio_offsets.size() && f == UINT32_MAX; ++i)
        for (uint32_t k = rec.audio_offsets[i]; k < rec.audio_offsets[i + 1]; ++k)
            if (rec.audio_hashes[k] == hash) { f = i; break; }
    if (f == UINT32_MAX) return 0;   // no aparece en la toma
    return capture_audio_window(rec, f);
}

// Preview por frame (el playhead): NO busca por hash — captura la mezcla del frame
// donde el usuario está parado (panel Capas). Es lo único fiable desde la UI: los
// hashes de audio del replay no coinciden con los grabados (ver [[audio-not-
// reproducible-on-replay]]), así que ubicar por hash grabado desde un occurrence
// del replay siempre fallaba (devolvía 0 → silencio).
size_t AytherSession::preview_audio_at(const AytherRecording& rec, uint32_t frame) {
    if (rec.empty()) return 0;
    const uint32_t n = rec.frame_count();
    if (frame >= n) return 0;
    return capture_audio_window(rec, frame);
}

// Escritor WAV mínimo (PCM S16 estéreo) — para el handoff de audio base (C-A4).
static bool write_wav_s16_stereo(const char* path, const int16_t* pcm,
                                 size_t frames, uint32_t rate) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    const uint32_t data_bytes = static_cast<uint32_t>(frames * 2u * sizeof(int16_t));
    auto u32 = [&](uint32_t v) { char b[4] = { char(v), char(v >> 8), char(v >> 16), char(v >> 24) }; f.write(b, 4); };
    auto u16 = [&](uint16_t v) { char b[2] = { char(v), char(v >> 8) }; f.write(b, 2); };
    f.write("RIFF", 4); u32(36u + data_bytes); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16u); u16(1); u16(2);              // PCM, 2 canales
    u32(rate); u32(rate * 2u * sizeof(int16_t));               // sample rate, byte rate
    u16(static_cast<uint16_t>(2u * sizeof(int16_t))); u16(16); // block align, bits/sample
    f.write("data", 4); u32(data_bytes);
    if (data_bytes) f.write(reinterpret_cast<const char*>(pcm), data_bytes);
    return static_cast<bool>(f);
}

size_t AytherSession::capture_audio_window(const AytherRecording& rec, uint32_t f) {
    return capture_pcm_span(rec, f, /*win=*/90, /*play=*/true);
}

size_t AytherSession::capture_pcm_span(const AytherRecording& rec, uint32_t f,
                                       uint32_t win, bool play, uint32_t mute_mask,
                                       size_t max_samples,
                                       const std::vector<uint64_t>* member_sigs,
                                       bool dynamic_mute) {
    Impl& im = *impl_;
    if (rec.empty() || f >= rec.frame_count()) return 0;

    // Guardar el estado actual (= el playhead) y el cursor de replay para
    // restaurarlos: la captura NO debe mover el playhead.
    std::vector<uint8_t> cur;
    if (!im.runner.serialize(cur) || cur.empty()) return 0;
    const int saved_pos = im.replay_pos;

    im.cap_active = true; im.cap_collect = false; im.cap_pcm.clear();

    // Posicionar la máquina en PRE-frame f (mejor keyframe ≤ f + bare).
    const std::vector<uint8_t>* state = nullptr;
    const uint32_t start = replay_start(rec, f, state);
    if (state && im.runner.unserialize(*state)) {
        for (uint32_t i = start; i < f; ++i) {
            im.runner.set_input(0, rec.inputs[i]);
            im.runner.run_frame();                  // bare, sin audio al device (cap_active)
        }
        // Acumular la MEZCLA de cada frame (no se filtra por hash: el audio del
        // replay no es byte-reproducible). `mute_mask` silencia canales SOLO
        // durante la ventana capturada (capture_channel_pcm aísla un canal).
        // `member_sigs` (Secuencias): aislamiento DINÁMICO por EVENTO — cada
        // frame deja sonar únicamente los canales donde un evento de esas
        // FIRMAS está activo (audio_events analizados); un golpe ajeno que
        // comparte canal con la melodía dentro del span ya no se cuela
        // (reporte 2026-07-23). Requiere análisis previo; sin él, fallback
        // al mute estático.
        im.cap_collect = true;
        const bool dyn = member_sigs && !member_sigs->empty() && !im.audio_events.empty();
        if (!dyn && mute_mask) im.runner.set_audio_mute_v1(mute_mask);
        const size_t kMaxSamples = max_samples ? max_samples
                                               : 44100u * 2u * 10u;   // default ~10 s
        const uint32_t n = rec.frame_count();
        for (uint32_t i = f; i < f + win && i < n; ++i) {
            // Mixdown del export MP4: la MISMA máscara dinámica que el playback
            // arma por frame (subs por evento/Secuencia + ocurrencias + manual)
            // → el «original muteado» sobre el que se mezclan los HD.
            if (dynamic_mute && !dyn)
                im.runner.set_audio_mute_v1(im.dynamic_audio_mute_at(i));
            if (dyn) {
                uint32_t allowed = 0, foreign_dac = 0;
                for (const auto& e : im.audio_events) {
                    if (i < e.start_frame || i > e.end_frame) continue;
                    const uint32_t bit = chan_bit(e.chip, e.channel);
                    const bool member = std::find(member_sigs->begin(), member_sigs->end(),
                                                  e.signature) != member_sigs->end();
                    if (member) { allowed |= bit; continue; }
                    // DAC AJENO activo (chip0 ch6, percusivo): el DAC comparte la
                    // salida física de FM6 y, encendido, PISA la nota FM6 — un
                    // grito/golpe ajeno se colaba por la ventana de una nota
                    // miembro de FM6 (reporte 2026-07-23, S26 en «Melodia»).
                    // Mientras suene, el canal se excluye (la nota miembro
                    // tampoco es audible ahí: el DAC la reemplaza).
                    if (e.chip == 0 && e.channel == 5 && e.pitch > 127)
                        foreign_dac |= bit;
                }
                allowed &= ~foreign_dac;
                im.runner.set_audio_mute_v1(kAllChannels & ~allowed);
            }
            im.runner.set_input(0, rec.inputs[i]);
            im.runner.run_frame();
            if (im.cap_pcm.size() >= kMaxSamples) break;
        }
        if (dyn || mute_mask || dynamic_mute) im.runner.set_audio_mute_v1(0);
        im.cap_collect = false;
    }

    // Drenar el tick_hits que ensució process_batch durante la captura.
    ayther_audio_hasher_end_tick(im.audio_hasher.get());
    im.cap_active = false;

    // Restaurar la máquina + el cursor (el playhead quedó intacto) y limpiar PCM staged.
    im.runner.unserialize(cur);
    im.replay_pos = saved_pos;
    im.audio.discard_emulator();

    const size_t frames = im.cap_pcm.size() / 2;
    if (play && im.audio_enabled && frames > 0)
        im.audio.play_oneshot_pcm(im.cap_pcm.data(), frames);
    return frames;
}

void AytherSession::play_pcm(const int16_t* pcm, size_t stereo_frames) {
    Impl& im = *impl_;
    if (im.audio_enabled && pcm && stereo_frames > 0)
        im.audio.play_oneshot_pcm(pcm, stereo_frames);
}

size_t AytherSession::capture_channel_pcm(const AytherRecording& rec, uint32_t start,
                                          uint32_t win, uint32_t solo_mask,
                                          std::vector<int16_t>& out) {
    Impl& im = *impl_;
    out.clear();
    const uint32_t mute = kAllChannels & ~solo_mask;
    // Toma COMPLETA (reporte 2026-07-21: el canal debe oírse entero, no 10 s).
    // Tope de seguridad ~5 min estéreo (~53 M samples ≈ 106 MB de int16).
    const size_t max_samples = 44100u * 2u * 300u;
    // : el aislamiento sale del espejo — lo de afuera del solo es cero
    // digital, no un mute que deja pasar el residuo de . Fallback a la
    // captura con máscara si no se puede (router off / toma sin cache).
    if (im.preview_render_base(start, win, max_samples / 2, solo_mask,
                               /*member_sigs=*/nullptr, out))
        return out.size() / 2;
    const size_t frames = capture_pcm_span(rec, start, win,
                                           /*play=*/false, mute, max_samples);
    if (frames > 0) out.assign(im.cap_pcm.begin(), im.cap_pcm.end());
    return frames;
}

size_t AytherSession::capture_events_pcm(const AytherRecording& rec, uint32_t start,
                                         uint32_t win,
                                         const std::vector<uint64_t>& member_sigs,
                                         std::vector<int16_t>& out) {
    Impl& im = *impl_;
    out.clear();
    if (member_sigs.empty()) return 0;
    const size_t max_samples = 44100u * 2u * 300u;
    // : aislamiento por EVENTO desde el espejo (ventana + cola de  del
    // lado de la fuente). Fallback a la captura dinámica si no se puede.
    if (im.preview_render_base(start, win, max_samples / 2, /*solo_mask=*/0,
                               &member_sigs, out))
        return out.size() / 2;
    // mute_mask 0: el aislamiento lo hace `member_sigs` por EVENTO — cada frame
    // deja sonar sólo los canales donde una de esas firmas está activa. Aislar
    // por canal dejaría entrar todo lo demás que comparte ese canal, que en el
    // Mega Drive es la música entera.
    const size_t frames = capture_pcm_span(rec, start, win, /*play=*/false,
                                           /*mute_mask=*/0, max_samples,
                                           &member_sigs);
    if (frames > 0) out.assign(im.cap_pcm.begin(), im.cap_pcm.end());
    return frames;
}

void AytherSession::preview_audio_span(const AytherRecording& rec, uint32_t start,
                                       uint32_t end, uint32_t solo_mask,
                                       const std::vector<uint64_t>* member_sigs,
                                       bool foreign_rec) {
    if (rec.empty() || start >= rec.frame_count()) return;
    if (end < start) end = start;
    // Tope 60 s (3600 frames) — el viejo 600 (~10 s) cortaba en seco las
    // Secuencias largas (melodías): «a partir de los 10 s ya no se escucha
    // nada» (reporte 2026-07-23).
    const uint32_t win = (std::min)(end - start + 1u, 3600u);
    // Rec AJENA a la toma analizada (reporte 2026-08-22: el ▶ original del
    // header de Secuencia desde otra toma): ni el espejo ni el aislamiento
    // por evento aplican — los dos salen del ANÁLISIS de la toma CARGADA y
    // sobre una rec ajena renderizarían otra música (o silencio). Re-sim de
    // la rec recibida con aislamiento por CANAL.
    if (foreign_rec) {
        const uint32_t mute = solo_mask ? (~solo_mask & kAllChannels) : 0u;
        capture_pcm_span(rec, start, win, /*play=*/true, mute,
                         /*max_samples=*/44100u * 2u * 60u, nullptr);
        return;
    }
    // : los aislamientos (canal o evento) salen del espejo — cero digital
    // fuera de lo pedido. La mezcla completa (0/null) sigue por la captura del
    // emulador: ahí el «original» es el chip real, como en el export hd=false.
    Impl& im = *impl_;
    if (solo_mask || (member_sigs && !member_sigs->empty())) {
        std::vector<int16_t> pcm;
        if (im.preview_render_base(start, win, /*max_frames=*/44100u * 60u,
                                   solo_mask, member_sigs, pcm)) {
            if (im.audio_enabled && pcm.size() >= 2)
                im.audio.play_oneshot_pcm(pcm.data(), pcm.size() / 2);
            return;
        }
    }
    // Aislamiento legacy: con `member_sigs` es POR EVENTO (dinámico, ver
    // capture_pcm_span); solo_mask por canal; 0/null = mezcla completa (compat).
    const uint32_t mute = solo_mask ? (~solo_mask & kAllChannels) : 0u;
    capture_pcm_span(rec, start, win, /*play=*/true, mute,
                     /*max_samples=*/44100u * 2u * 60u, member_sigs);
}

bool AytherSession::export_mixdown_wav(const AytherRecording& rec, uint32_t start,
                                       uint32_t end, const char* wav_path, bool hd) {
    Impl& im = *impl_;
    if (rec.empty() || !wav_path || wav_path[0] == '\0') return false;
    if (start >= rec.frame_count()) return false;
    if (end > rec.frame_count()) end = rec.frame_count();
    if (end <= start) return false;
    const double   fps  = timing_fps() > 1.0 ? timing_fps() : 60.0;
    // Rango [start, end), tope duro ~15 min de juego.
    const uint32_t win  = (std::min)(end - start, 54000u);
    const size_t   max_samples =
        static_cast<size_t>((win / fps + 2.0) * 44100.0) * 2u;

    // Base: original puro (hd=false), o la COMPOSICIÓN (hd=true). El gate
    // audio_sub_preview se FUERZA: el mixdown es «como suena en Mezclar», no
    // depende del toggle Assets del momento del export.
    //
    // : con el router puesto, la base hd sale del RENDER OFFLINE del
    // router (export_router_base) — las mismas decisiones que la preview,
    // SoundFont incluido, que el camino viejo directamente PERDÍA (muteaba la
    // voz original y no mezclaba la re-síntesis en ningún lado). El camino
    // viejo (máscara sustractiva sobre el emulador) queda como fallback:
    // router apagado, toma sin analizar o cache de escrituras capped.
    // El export hd=false NO cambia: el original es el chip real del juego.
    const bool saved_preview = im.audio_sub_preview;
    if (hd) im.audio_sub_preview = true;
    size_t frames = 0;
    if (hd && im.export_router_base(start, win, fps, max_samples / 2, im.cap_pcm))
        frames = im.cap_pcm.size() / 2;
    if (frames == 0)
        frames = capture_pcm_span(rec, start, win, /*play=*/false,
                                  /*mute_mask=*/0, max_samples,
                                  /*member_sigs=*/nullptr,
                                  /*dynamic_mute=*/hd);
    im.audio_sub_preview = saved_preview;
    if (frames == 0) return false;

    if (!hd)
        return write_wav_s16_stereo(wav_path, im.cap_pcm.data(), frames, 44100u);

    // ---- Mezcla de los HD encima (acumulador i32 con clamp) ----------------
    std::vector<int32_t> mix(im.cap_pcm.begin(),
                             im.cap_pcm.begin() +
                                 static_cast<std::ptrdiff_t>(frames * 2));
    // Cache local de decodes convertidos (varias subs comparten asset).
    std::unordered_map<std::string, std::vector<int16_t>> decoded;
    auto pcm_of = [&](const std::string& asset) -> const std::vector<int16_t>& {
        auto it = decoded.find(asset);
        if (it == decoded.end()) {
            std::vector<int16_t> p;
            im.audio.decode_asset_pcm_s16_44k(asset, p);
            it = decoded.emplace(asset, std::move(p)).first;
        }
        return it->second;
    };
    // Mezcla una serie de instancias del MISMO origen: cada ancla (frame de
    // juego) dispara el asset; el ancla siguiente TRUNCA la instancia previa
    // (el retrigger del playback reinicia el stream) con fade ~5 ms para no
    // clickear; `looping` repite el PCM hasta cubrir `duration_frames`.
    constexpr int64_t kFade = 220;   // cuadros de 44.1 kHz ≈ 5 ms
    //
    // `muted` marca las instancias SILENCIADAS (). Van igual en `anchors`
    // aunque no suenen: el ancla siguiente trunca a la anterior, así que
    // sacarlas de la lista alargaría la instancia previa hasta la subsiguiente.
    auto mix_series = [&](const std::vector<uint32_t>& anchors,
                          const std::vector<uint8_t>& muted,
                          const std::vector<int64_t>& max_len,
                          const std::vector<int16_t>& pcm, float gain,
                          bool looping, uint32_t duration_frames) {
        const int64_t pcm_frames = static_cast<int64_t>(pcm.size() / 2);
        if (pcm_frames == 0) return;
        const int64_t total = static_cast<int64_t>(frames);
        for (size_t k = 0; k < anchors.size(); ++k) {
            if (k < muted.size() && muted[k]) continue;
            // Largo de la instancia en CUADROS de audio.
            int64_t len = looping
                ? static_cast<int64_t>(duration_frames / fps * 44100.0)
                : pcm_frames;
            bool truncated = false;
            // : tope de la VENTANA (end + tail, en cuadros de audio) — el
            // export corta donde corta el vivo; <=0 = sin tope (legacy).
            if (k < max_len.size() && max_len[k] > 0 && max_len[k] < len) {
                len = max_len[k];
                truncated = true;   // fade corto: mismo declick que el retrigger
            }
            if (k + 1 < anchors.size()) {
                const int64_t cut = static_cast<int64_t>(
                    (anchors[k + 1] - anchors[k]) / fps * 44100.0);
                if (cut < len) { len = cut; truncated = true; }
            }
            const int64_t s0 = static_cast<int64_t>(
                (static_cast<int64_t>(anchors[k]) -
                 static_cast<int64_t>(start)) / fps * 44100.0);
            if (s0 >= total || s0 + len <= 0) continue;   // fuera del rango
            for (int64_t i = 0; i < len; ++i) {
                const int64_t at = s0 + i;
                if (at < 0) continue;
                if (at >= total) break;
                const int64_t src = looping ? (i % pcm_frames) : i;
                if (src >= pcm_frames) break;
                float g = gain;
                if (truncated && i >= len - kFade)
                    g *= static_cast<float>(len - i) / static_cast<float>(kFade);
                mix[static_cast<size_t>(at) * 2 + 0] += static_cast<int32_t>(
                    pcm[static_cast<size_t>(src) * 2 + 0] * g);
                mix[static_cast<size_t>(at) * 2 + 1] += static_cast<int32_t>(
                    pcm[static_cast<size_t>(src) * 2 + 1] * g);
            }
        }
    };

    // Secuencias: anclas con la MISMA segmentación greedy de seq_sub_anchor
    // (paso = span de eventos), SIN filtrar por rango — el ancla siguiente
    // trunca aunque caiga fuera; las instancias fuera se saltan en mix_series.
    for (const auto& sq : im.audio_seq_subs) {
        if (sq.asset.empty()) continue;
        std::vector<uint32_t> anchors;
        std::vector<uint8_t>  muted;
        std::vector<int64_t>  max_len;
        // : las anclas salen de la MISMA tabla conjunta del playback
        // (segmentación greedy + reclamo entre Secuencias).
        for (const uint32_t a : im.seq_anchors_of(sq)) {
            anchors.push_back(a);
            // El mixdown es lo que se EXPORTA (el WAV del MP4): sin esto, la
            // fuga del  sobrevivía al render aunque el playback ya la
            // hubiera tapado — el original muteado y el HD igual de presente.
            muted.push_back(im.seq_sub_muted(sq, a) ? 1u : 0u);
            // : la ventana de la Secuencia es su contrato audible — el
            // vivo corta al expirar y el export corta en la misma muestra.
            max_len.push_back(sq.duration_frames > 0
                ? static_cast<int64_t>(sq.duration_frames / fps * 44100.0)
                : 0);
        }
        // El HD suena su ventana completa; con looping repite hasta cubrirla.
        mix_series(anchors, muted, max_len, pcm_of(sq.asset), sq.gain,
                   /*looping=*/false, sq.duration_frames);
    }
    // Asignaciones per-firma: el playback re-dispara en CADA ocurrencia que
    // RESUELVE a la asignación — firma exacta o regla de instrumento (
    // F3), el mismo criterio del disparo en vivo — un ancla por ocurrencia,
    // asset a gain 1.
    {
        struct EvAnchors {
            std::vector<uint32_t> anchors;
            std::vector<uint8_t>  muted;
            std::vector<int64_t>  max_len;
        };
        std::unordered_map<uint64_t, EvAnchors> by_assign;
        for (const auto& e : im.audio_events) {
            uint64_t asig = e.signature;
            if (!im.resolve_event_sig(e.signature, e.instrument, e.pitch,
                                      &asig)) continue;
            const auto it = im.audio_event_assign.find(asig);
            if (it == im.audio_event_assign.end() || it->second.empty())
                continue;
            EvAnchors& ea = by_assign[asig];
            if (!ea.anchors.empty() && ea.anchors.back() == e.start_frame)
                continue;
            ea.anchors.push_back(e.start_frame);
            ea.muted.push_back(im.event_muted(e) ? 1u : 0u);
            // : tail FINITO acota cada ocurrencia a su ventana + tail
            // (misma muestra que el barrido del vivo); legacy = sin tope.
            const uint32_t tail = im.tail_of(asig);
            ea.max_len.push_back(tail == Impl::kTailUnlimited
                ? 0
                : static_cast<int64_t>(
                      (e.end_frame - e.start_frame + 1 + tail) / fps *
                      44100.0));
        }
        for (auto& [asig, ea] : by_assign)
            mix_series(ea.anchors, ea.muted, ea.max_len,
                       pcm_of(im.audio_event_assign.at(asig)), 1.0f, false, 0);
    }

    std::vector<int16_t> outp(frames * 2);
    for (size_t i = 0; i < outp.size(); ++i)
        outp[i] = static_cast<int16_t>(std::clamp(mix[i], -32768, 32767));
    return write_wav_s16_stereo(wav_path, outp.data(), frames, 44100u);
}

bool AytherSession::export_audio_event_wav(const AytherRecording& rec, uint32_t start,
                                           uint32_t end, const char* wav_path, uint32_t tail,
                                           uint32_t solo_mask,
                                           const std::vector<uint64_t>* member_sigs) {
    if (rec.empty() || !wav_path || wav_path[0] == '\0') return false;
    if (start >= rec.frame_count()) return false;
    if (end < start) end = start;
    // Ventana = span + `tail` frames de cola (0 = exacto al span), con tope.
    const uint32_t span = end - start + 1;
    // Tope ~5 min (el Exportar unificado permite la TOMA COMPLETA en WAV).
    const uint32_t win  = (std::min)(span + tail, 18000u);
    // Aislamiento idéntico al preview de Secuencia (reporte 2026-07-23: el WAV
    // exportado traía sonidos ajenos al span): por EVENTO con `member_sigs`,
    // por canal con `solo_mask`, mezcla completa sin ambos.
    //
    // : los aislados salen del espejo (cero digital fuera del pedido) —
    // este WAV es un ENTREGABLE y arrastraba el residuo de .
    Impl& im = *impl_;
    if (solo_mask || (member_sigs && !member_sigs->empty())) {
        if (im.preview_render_base(start, win, (44100u * 2u * 300u) / 2,
                                   solo_mask, member_sigs, im.cap_pcm))
            return write_wav_s16_stereo(wav_path, im.cap_pcm.data(),
                                        im.cap_pcm.size() / 2, 44100u);
    }
    const uint32_t mute = solo_mask ? (~solo_mask & kAllChannels) : 0u;
    const size_t frames = capture_pcm_span(rec, start, win, /*play=*/false,
                                           /*mute_mask=*/mute,
                                           /*max_samples=*/44100u * 2u * 300u,
                                           member_sigs);
    if (frames == 0) return false;
    return write_wav_s16_stereo(wav_path, impl_->cap_pcm.data(), frames, 44100u);
}

bool AytherSession::export_channel_wav(const AytherRecording& rec, uint32_t start,
                                       uint32_t end, uint32_t solo_mask, uint32_t tail,
                                       const char* wav_path) {
    if (rec.empty() || !wav_path || wav_path[0] == '\0') return false;
    if (start >= rec.frame_count()) return false;
    if (end < start) end = start;
    // Sample corto (un sonido, no la toma): tope 600 frames ≈ 10 s.
    const uint32_t win = (std::min)(end - start + 1u + tail, 600u);
    std::vector<int16_t> pcm;
    const size_t frames = capture_channel_pcm(rec, start, win, solo_mask, pcm);
    if (frames == 0) return false;
    return write_wav_s16_stereo(wav_path, pcm.data(), frames, 44100u);
}

// ---------------------------------------------------------------------------
// Recording + replay (R7)
// ---------------------------------------------------------------------------
void AytherSession::record_start() {
    Impl& im = *impl_;
    im.rec_inputs.clear();
    im.rec_stats.clear();
    im.rec_hashes.clear();
    im.rec_hash_off.assign(1, 0);   // CSR starts with a single 0 boundary
    im.rec_audio_hashes.clear();
    im.rec_audio_off.assign(1, 0);  // CSR de audio: idem (.arp v7)
    im.rec_keyframes.clear();       // R7e: keyframes horneados de esta toma
    im.rec_active = im.runner.serialize(im.rec_initial);   // capture the initial state
    if (!im.rec_active)
        std::fprintf(stderr, "[AytherSession] record_start: serialize failed\n");
}

void AytherSession::record_stop() { impl_->rec_active = false; }

bool   AytherSession::recording()        const noexcept { return impl_->rec_active; }
size_t AytherSession::recorded_frames()  const noexcept { return impl_->rec_inputs.size(); }

AytherRecording AytherSession::take_recording() {
    Impl& im = *impl_;
    AytherRecording rec;
    rec.game_id       = game_id();
    rec.initial_state = im.rec_initial;       // copy (the take outlives the buffer)
    rec.inputs        = im.rec_inputs;
    rec.stats         = im.rec_stats;
    rec.sprite_hashes = im.rec_hashes;
    rec.hash_offsets  = im.rec_hash_off;   // already CSR with frame_count()+1 entries
    rec.audio_hashes  = im.rec_audio_hashes;
    rec.audio_offsets = im.rec_audio_off;  // CSR de audio (.arp v7)
    rec.trim_in       = 0;
    rec.trim_out      = rec.frame_count();
    // R7e: hornear los keyframes captados (comprimidos) en el .arp.
    for (auto& [frame, st] : im.rec_keyframes) rec.add_keyframe(frame, st);
    im.rec_keyframes.clear();
    im.rec_active = false;
    return rec;
}

// Captura un keyframe RAW del estado vivo en replay_keys[key] si cae en frontera
// y no existe. PRECONDICIÓN: el estado debe haber pasado por produce_frame (post-
// produce reproduce; post-bare diverge — ver split_smoke). key = frame al que el
// estado da arranque (machine tras producir frame key-1 == start-state de key).
void AytherSession::replay_capture_key(uint32_t key) {
    Impl& im = *impl_;
    // R7e: si la toma trae keyframes horneados, NO acumulamos runtime (los
    // horneados ya cubren todo el rango → RAM acotada).
    if (im.replay_rec && !im.replay_rec->keyframes.empty()) return;
    if (key % kReplayKeyInterval != 0) return;
    if (im.replay_rec && key >= im.replay_rec->frame_count()) return;
    if (im.replay_keys.count(key)) return;
    std::vector<uint8_t> st;
    if (im.runner.serialize(st) && !st.empty())
        im.replay_keys.emplace(key, std::move(st));
}

// R7e: mejor arranque para un seek a `target`. El mayor entre el estado inicial
// (frame 0), el keyframe runtime más cercano (replay_keys, crudo) y el keyframe
// horneado más cercano (rec.keyframes, comprimido → descomprimido a kf_scratch).
uint32_t AytherSession::replay_start(const AytherRecording& rec, uint32_t target,
                                     const std::vector<uint8_t>*& state) {
    Impl& im = *impl_;
    uint32_t start = 0;
    state = &rec.initial_state;
    if (im.replay_rec == &rec && !im.replay_keys.empty()) {
        auto it = im.replay_keys.upper_bound(target);
        if (it != im.replay_keys.begin()) {
            --it;
            if (it->first > start) { start = it->first; state = &it->second; }
        }
    }
    if (!rec.keyframes.empty()) {
        auto it = std::upper_bound(
            rec.keyframes.begin(), rec.keyframes.end(), target,
            [](uint32_t t, const AytherRecording::Keyframe& k) { return t < k.frame; });
        if (it != rec.keyframes.begin()) {
            --it;
            if (it->frame > start &&
                rec.decompress_keyframe(static_cast<size_t>(it - rec.keyframes.begin()),
                                        im.kf_scratch)) {
                start = it->frame;
                state = &im.kf_scratch;
            }
        }
    }
    return start;
}

uint32_t AytherSession::replay_start_frame(const AytherRecording& rec, uint32_t target) const {
    const Impl& im = *impl_;
    uint32_t start = 0;
    if (im.replay_rec == &rec && !im.replay_keys.empty()) {
        auto it = im.replay_keys.upper_bound(target);
        if (it != im.replay_keys.begin()) { --it; if (it->first > start) start = it->first; }
    }
    if (!rec.keyframes.empty()) {
        auto it = std::upper_bound(
            rec.keyframes.begin(), rec.keyframes.end(), target,
            [](uint32_t t, const AytherRecording::Keyframe& k) { return t < k.frame; });
        if (it != rec.keyframes.begin()) { --it; if (it->frame > start) start = it->frame; }
    }
    return start;
}

// Replay seek (R7d): renderiza `frame` de `rec` re-simulando lo MÍNIMO posible,
// SIN cambiar el resultado observable. Dos caminos, ambos respetando el primitivo
// que valida split_smoke (unserialize + cadena bare pura + produce del visible):
//   1. Playback secuencial (target == cursor+1): cadena PURA de produce desde el
//      estado vivo — la grabación misma es una cadena de produce (step), así que
//      continuarla con produce la reproduce. O(1) por frame.
//   2. Salto arbitrario: unserialize del keyframe cacheado más cercano ≤ target
//      (o estado inicial) + bare hasta target + produce. Acota el re-sim a
//      ≤ kReplayKeyInterval frames una vez que la región tiene keyframes.
// NUNCA se mezcla "bare después de un produce a media cadena": produce y run_frame
// bare dejan estado oculto distinto y mezclarlos sin round-trip de serialización
// desincroniza el framebuffer (lo verificó split_smoke al fallar). Por eso los
// keyframes se capturan SOLO tras el produce terminal (= patrón del tail de split,
// que reproduce), y el playback —que produce cada frame— los puebla densamente.
const FrameView* AytherSession::replay_seek(const AytherRecording& rec, uint32_t frame,
                                            bool quiet) {
    Impl& im = *impl_;
    std::fprintf(stderr, "[dbgsk] replay_seek f=%u pos=%d quiet=%d\n",
                 frame, im.replay_pos, (int)quiet);
    if (rec.empty()) return nullptr;
    im.chunk.active = false;   // un seek directo supersede cualquier seek en chunks

    // Grabación distinta → cache obsoleto. (El caller también llama replay_reset()
    // al cargar/dividir, donde el objeto se reusa con otro contenido.)
    if (im.replay_rec != &rec) {
        im.replay_rec = &rec;
        im.replay_keys.clear();
        im.replay_pos = -1;
    }

    const uint32_t target = frame < rec.frame_count() ? frame : rec.frame_count() - 1;
    // Salto = todo lo que no sea el avance de a un frame del playback. Lo
    // consume el reconocimiento del Cuadro (ver screen_jump).
    im.screen_jump = im.replay_pos < 0 ||
                     target != static_cast<uint32_t>(im.replay_pos + 1);
    if (im.replay_pos == static_cast<int>(target))
        return &im.view;   // ya posicionado: la vista del último produce sigue válida

    // Camino rápido: avance CORTO hacia adelante continuando la cadena — bare
    // para los intermedios (su audio queda staged y sale con el flush del
    // produce final: el catch-up NO pierde PCM) + produce del frame visible.
    //  (la "espiral del catch-up"): antes solo cubría target == pos+1 —
    // cualquier adv≥2 del catch-up del Lab caía al camino general (unserialize
    // del keyframe + re-sim de TODA la distancia, hasta ~300 frames ≈ 120-300
    // ms POR TICK — medido seek=127ms con adv=7) y además DESCARTABA el PCM de
    // los frames alcanzados: un stall (maximizar, cargar texturas, perder el
    // foco) generaba deuda → adv≥2 → tick carísimo → más deuda → espiral
    // auto-sostenida con el audio hambreado hasta cruzar un keyframe.
    constexpr uint32_t kFastForwardMax = 32;   // > kMaxCatchup del Lab (16)
    if (im.replay_pos >= 0 && target > static_cast<uint32_t>(im.replay_pos)
        && target - static_cast<uint32_t>(im.replay_pos) <= kFastForwardMax) {
        for (uint32_t i = static_cast<uint32_t>(im.replay_pos) + 1; i < target; ++i) {
            // Catch-up del playback: el PCM de estos frames SE CONSERVA (va al
            // device) → con el camino VIEJO deben correr con el mismo mute
            // dinámico que un produce, o la Secuencia deshabilitada/sustituida
            // se escucha A PLENO en cada ráfaga de deuda (reporte 2026-07-23).
            // No es poco: el Lab avanza ~6 frames por tick, así que este bucle
            // produce seis de cada siete frames de audio que van al device.
            //
            // Con el ROUTER no hace falta muteo acá: el `buffer_router` del
            // produce final pisa TODO lo staged —lo de estos frames incluido— y
            // lo reemplaza por su mezcla, que cubre la misma duración. Y no
            // muteando, el hasher los ve.
            //
            // : salvo en Sega CD, donde el bloque del router se SUMA y lo
            // staged sobrevive. Ahí estos frames van al device tal cual salen
            // del chip, así que necesitan las dos máscaras: la dinámica —o la
            // Secuencia sustituida se oye a pleno en cada ráfaga, que es el
            // reporte de arriba— y los canales que el router rinde, o el FM y
            // el PSG originales se oirían debajo de su propio espejo.
            if (!quiet && (!im.voice_router_on || im.router_mix()))
                im.runner.set_audio_mute_v1(
                    im.dynamic_audio_mute_at(i) |
                    (im.voice_router_on ? kRouterChannels : 0u));
            // El espejo del router necesita TODAS las escrituras, y las de estos
            // frames bare se perderían: produce_frame resetea el log al empezar.
            // Sin esto se le escapan los key-on y los cambios de patch de hasta
            // 31 frames y el timbre queda viejo.
            im.runner.set_input(0, rec.inputs[i]);
            im.audio.mark_frame_boundary();     // : offset del frame i
            im.runner.run_frame();              // bare; audio staged (no se pierde)
            if (im.voice_router_on) {
                // E-5 (): el reset manual del log se retiró — con ABI el
                // core lo cierra en SU frame boundary, que es justo lo que este
                // bucle necesitaba: que las escrituras de CADA frame bare
                // lleguen enteras, sin arrastrar las del anterior.
                ayther_frame_snapshot_v1 bs{};
                if (im.runner.capture_frame_snapshot(bs).ok()) {
                    im.abi_audio.resize(bs.audio_write_count);
                    const auto rb = im.runner.read_audio_writes_v1(
                        im.abi_audio.data(),
                        static_cast<uint32_t>(im.abi_audio.size()), bs);
                    if (rb.ok())
                        im.voice_capture(
                            reinterpret_cast<const AytherAudioWrite*>(
                                im.abi_audio.data()), rb.count, i);
                } else {
                    // Sin ABI: el log acumula (ya no hay reset que lo corte),
                    // así que el router ve de más. El router exige el fork de
                    // todos modos — las escrituras de chip son un id privado.
                    AYTHER_LEGACY_READ_BEGIN
                    im.voice_capture(
                        reinterpret_cast<const AytherAudioWrite*>(
                            im.runner.audio_writes()),
                        im.runner.audio_write_count(), i);
                    AYTHER_LEGACY_READ_END
                }
            }
        }
        if (!quiet) im.runner.set_audio_mute_v1(0);
        // Scrub del usuario (`quiet`): descartar el PCM staged de los
        // intermedios — conservarlo es solo para el catch-up del playback; en
        // un scrub encola audio a 1× y se desfasa del cabezal. El produce del
        // frame visible conserva SU audio (blip que sigue al cabezal).
        if (quiet && im.audio_enabled) im.audio.discard_emulator();
        im.runner.set_input(0, rec.inputs[target]);
        im.frame_index = target;
        const FrameView& v = produce_frame();
        im.replay_pos = static_cast<int>(target);
        replay_capture_key(target + 1);
        return &v;
    }

    // Camino general: arranca del mejor keyframe ≤ target (runtime u horneado, o
    // estado inicial) y re-simula el resto con run_frame "bare" puro.
    //
    // El cebado del espejo del router (voice_prime_to) necesita las escrituras
    // de TODA la toma, y hasta ahora solo las cacheaba analyze_audio_events —
    // una toma con audio_events.toml persistido nunca re-analiza y el caché
    // quedaba VACÍO: el espejo arrancaba en frío tras el salto, sin los
    // patches FM del inicio de la canción (que no se reescriben nunca) y el
    // título quedaba fino/agudo para siempre (reporte 2026-08-19). Se
    // construye acá, UNA vez por toma (pasada bare, ~1-2 s; la máquina se
    // reposiciona igual justo después). En quiet (análisis/bake) no: el
    // análisis está construyendo el caché él mismo.
    if (im.voice_router_on && !im.replay_quiet) {
        // : cebado INCREMENTAL hasta el target (no toda la toma). Mejor
        // arranque para el tramo faltante: el savestate propio del caché si
        // está justo al final de lo construido; si no, el mejor keyframe ≤
        // built (runtime/horneado/inicial), corriendo bare lo ya cacheado.
        if (im.voice_prime_rec != static_cast<const void*>(&rec)) im.voice_prime_reset(rec);
        const uint32_t built = im.voice_prime_built();
        if (!im.voice_prime_capped && built < (std::min)(target + 1, rec.frame_count())) {
            const std::vector<uint8_t>* st = nullptr;
            uint32_t st_frame = 0;
            if (!im.voice_prime_state.empty() && im.voice_prime_state_frame == built) {
                st = &im.voice_prime_state; st_frame = built;
            } else {
                st_frame = replay_start(rec, built, st);
            }
            // Copia: voice_prime_build puede pisar voice_prime_state al final.
            const std::vector<uint8_t> st_copy = st ? *st : std::vector<uint8_t>{};
            im.voice_prime_build(rec, target + 1, st_frame, &st_copy);
        }
    }
    // Seek NO secuencial → descartar los tracks de tween (sin esto un scrub
    // deja transiciones fantasma de la posición anterior) y las ventanas de
    // secuencia de audio (mismo motivo). El fast-forward de arriba es continuo
    // y conserva el estado.
    if (im.tween) ayther_tween_clear(im.tween.get());
    im.audio_seq_windows.clear();
    im.audio_seq_fired.clear();     // salto: re-pasar una ventana debe re-disparar
    im.audio_evt_fired.clear();
    // Salto = el cabezal se movió: CORTAR los HD en el aire (limpiar los
    // `fired` sin cortar dejaba el one-shot viejo sonando superpuesto con el
    // del destino — el eco del reporte 2026-07-23).
    if (im.audio_enabled) { im.audio.stop_all_sfx(); im.audio.stop_all_events(); }
    const std::vector<uint8_t>* state = nullptr;
    const uint32_t start = replay_start(rec, target, state);
    if (!im.runner.unserialize(*state)) { im.replay_pos = -1; return nullptr; }
    for (uint32_t i = start; i < target; ++i) {
        im.runner.set_input(0, rec.inputs[i]);
        im.runner.run_frame();              // bare, rápido y silencioso
    }

    // Descarta el PCM acumulado por los bare → el seek no "estalla" audio. Un
    // SALTO también invalida los event-streams HD en curso (C-A2): el audio de
    // un evento arrancado en otra parte de la línea de tiempo ya no corresponde.
    if (im.audio_enabled) {
        im.audio.discard_emulator();
        im.audio.stop_all_events();
    }

    im.runner.set_input(0, rec.inputs[target]);
    im.frame_index = target;
    const FrameView& v = produce_frame();   // frame visible (con su audio)
    im.replay_pos = static_cast<int>(target);
    replay_capture_key(target + 1);         // keyframe (post-produce) para futuros saltos
    return &v;
}

// ---------------------------------------------------------------------------
// Eventos de audio por comandos de chip (C-A2). Reproduce la toma una vez hacia
// adelante (replay_seek secuencial = produce cada frame, que llena chip_writes en
// la FrameView) y alimenta el AudioEventDetector. El log de escrituras es
// replay-determinista (tools/audio_chip_spike), así que los eventos son estables.
// Silencioso: replay_quiet evita que el fast-forward "estalle" audio al device.
// ---------------------------------------------------------------------------
uint32_t AytherSession::analyze_audio_events(const AytherRecording& rec) {
    Impl& im = *impl_;
    im.audio_events.clear();
    if (rec.empty() || !im.audio_event_det) return 0;

    ayther_audio_event_reset(im.audio_event_det.get());
    // Región del reloj para el pitch (Mezclar): PAL si el core corre <55 fps.
    const uint8_t pal = timing_fps() > 1.0 && timing_fps() < 55.0 ? 1 : 0;
    ayther_audio_event_set_pal(im.audio_event_det.get(), pal);
    if (im.audio_live_det) ayther_audio_event_set_pal(im.audio_live_det.get(), pal);

    // Evidencia de AUDIO para los eventos residuales: sonda corta de PCM por
    // canal AISLADO al arranque de la toma → máscara de canales que SUENAN.
    // Sin esto, el detector no puede distinguir un key-off/mute de INIT sobre
    // un canal silencioso (residual espurio) de una nota real del estado
    // inicial — las escrituras no alcanzan: GA escribe frecuencias PSG aunque
    // el canal calle (reporte 2026-07-21).
    {
        uint16_t active0 = 0;
        std::vector<int16_t> probe;
        for (int b = 0; b < 10; ++b) {
            const uint16_t solo = static_cast<uint16_t>(1u << b);
            const size_t fr = capture_channel_pcm(rec, 0, 8, solo, probe);
            if (fr == 0) continue;
            double acc = 0.0;
            const size_t ns = fr * 2;
            for (size_t s = 0; s < ns; ++s)
                acc += std::abs(static_cast<double>(probe[s]));
            if (acc / static_cast<double>(ns) > 40.0) active0 |= solo;
        }
        ayther_audio_event_set_initial_active(im.audio_event_det.get(), active0);
    }

    const bool prev_quiet = im.replay_quiet;
    im.replay_quiet = true;                 // el análisis no debe sonar
    const uint32_t n = rec.frame_count();
    // Esta pasada es la ÚNICA que ve las escrituras de toda la toma (la
    // grabación guarda inputs, no escrituras). El router las necesita para
    // reconstruir el estado del espejo tras un seek, así que se cachean acá en
    // vez de re-emular después. Ver voice_prime_to.
    im.voice_prime_reset(rec);   // el análisis también es dueño válido del caché
    for (uint32_t f = 0; f < n; ++f) {
        const FrameView* v = replay_seek(rec, f);   // secuencial → produce cada frame
        if (v)
            // Los DOS caminos, igual que en vivo (). `im.pcm_events` lo
            // acaba de llenar el produce de este mismo frame: sin esto se puede
            // sustituir el chip PCM en vivo pero NO autorarlo, que es como se
            // trabaja — el usuario asigna sobre los eventos de una toma
            // analizada, y ahí el chip no aparecía.
            ayther_audio_event_process_frame_ex(
                im.audio_event_det.get(), f, v->chip_writes, v->chip_write_count,
                im.pcm_events.data(), static_cast<uint32_t>(im.pcm_events.size()));
        // : el produce de este frame ya pudo empujar sus escrituras al
        // caché (voice_capture → voice_prime_push, contiguo e idempotente);
        // si el router está apagado, se empujan acá con la misma primitiva.
        if (v) im.voice_prime_push(f, v->chip_writes, v->chip_write_count);
        else   im.voice_prime_push(f, nullptr, 0);
    }
    im.replay_quiet = prev_quiet;

    ayther_audio_event_finish(im.audio_event_det.get());

    const uint32_t k = ayther_audio_event_count(im.audio_event_det.get());
    im.audio_events.resize(k);
    if (k) ayther_audio_event_get(im.audio_event_det.get(), im.audio_events.data(), k);
    return k;
}

const AytherAudioEvent* AytherSession::audio_events() const noexcept {
    return impl_->audio_events.empty() ? nullptr : impl_->audio_events.data();
}
uint32_t AytherSession::audio_event_count() const noexcept {
    return static_cast<uint32_t>(impl_->audio_events.size());
}
void AytherSession::clear_audio_events() noexcept { impl_->audio_events.clear(); }

// -- Sustitución de audio por evento (C-A3b) --------------------------------
void AytherSession::assign_audio_event(uint64_t signature, const char* asset_path) {
    impl_->audio_event_assign[signature] = asset_path ? asset_path : "";
    // : asignar (o re-asignar) re-arma la transacción — el próximo disparo
    // reintenta aunque el anterior haya fallado el arranque.
    impl_->hd_failed_keys.erase(signature);
    // : la instancia live en vuelo apunta al asset VIEJO — reanudarla
    // sonaría lo que el artista acaba de reemplazar; muere acá y el próximo
    // key-on nace con el nuevo.
    impl_->audio_live_inst.erase(signature);
    // Prewarm: decodificar YA (la carga del proyecto pasa por acá) para que el
    // primer disparo en playback no pague el decode (stall → catch-up → el
    // trigger se saltea y el sonido no suena la primera vez).
    if (asset_path && *asset_path &&
        impl_->audio_prewarmed.insert(asset_path).second)
        impl_->audio.prewarm_asset_file(asset_path);
    //  F3: una regla cargada antes que su asignación (orden del TOML)
    // recién puede indexarse ahora.
    impl_->rebuild_match_index();
}
void AytherSession::unassign_audio_event(uint64_t signature) {
    impl_->audio_event_assign.erase(signature);
    impl_->hd_failed_keys.erase(signature);
    impl_->audio_event_channels.erase(signature);
    impl_->audio_event_duration.erase(signature);
    impl_->audio_event_looping.erase(signature);
    impl_->audio_event_tail.erase(signature);
    impl_->hd_oneshot_cut.erase(signature);
    impl_->audio_live_inst.erase(signature);   // : sin asignación no hay reemplazo
    impl_->audio_event_rule.erase(signature);  //  F3: la regla vive con ella
    impl_->rebuild_match_index();
}
void AytherSession::clear_audio_event_assignments() noexcept {
    impl_->audio_event_assign.clear();
    impl_->hd_failed_keys.clear();
    impl_->audio_event_channels.clear();
    impl_->audio_event_duration.clear();
    impl_->audio_event_looping.clear();
    impl_->audio_event_members.clear();
    impl_->audio_event_head.clear();       // 
    impl_->audio_event_seq_next.clear();
    impl_->audio_event_tail.clear();
    impl_->hd_oneshot_cut.clear();
    impl_->audio_event_rule.clear();      //  F3
    impl_->audio_match_index.clear();
    impl_->audio_seq_windows.clear();
    // : se van las asignaciones de evento — las instancias de Secuencia
    // (seq_sub) viven de audio_seq_subs y su ciclo las cierra por su lado.
    for (auto it = impl_->audio_live_inst.begin();
         it != impl_->audio_live_inst.end(); )
        it = it->second.seq_sub ? std::next(it) : impl_->audio_live_inst.erase(it);
}
// Lista (firma, asset, canales) de las asignaciones — para la entrega .ay. Los
// canales se toman de los eventos vivos (si se analizó) o del mapa cargado.
std::vector<AytherSession::AudioEventSub> AytherSession::audio_event_subs() const {
    const Impl& im = *impl_;
    std::unordered_map<uint64_t, uint32_t> derived;
    for (const auto& e : im.audio_events) {
        const uint32_t bit = chan_bit(e.chip, e.channel);
        derived[e.signature] |= bit;
    }
    std::vector<AudioEventSub> out;
    out.reserve(im.audio_event_assign.size());
    for (const auto& [sig, asset] : im.audio_event_assign) {
        uint32_t ch = 0;
        if (const auto it = derived.find(sig); it != derived.end()) ch = it->second;
        if (ch == 0) {
            if (const auto it = im.audio_event_channels.find(sig);
                it != im.audio_event_channels.end()) ch = it->second;
        }
        AudioEventSub sub{ sig, asset, ch, 0, false };
        if (const auto d = im.audio_event_duration.find(sig);
            d != im.audio_event_duration.end())
            sub.duration_frames = d->second;
        if (const auto l = im.audio_event_looping.find(sig);
            l != im.audio_event_looping.end())
            sub.looping = l->second;
        sub.tail_frames = im.tail_of(sig);   //  (kTailUnlimited = no autorado)
        if (const auto r = im.audio_event_rule.find(sig);
            r != im.audio_event_rule.end()) {   //  F3
            sub.match_rule       = r->second.rule;
            sub.match_instrument = r->second.instrument;
            sub.match_pitch      = r->second.pitch;
        }
        out.push_back(std::move(sub));
    }
    return out;
}

//  F3: la regla se ARMA con la identidad del timbre en la mano — de los
// eventos analizados (Mezclar) o de lo aprendido por el detector live
// (Capturar) — y se persiste con la asignación; nunca se re-infiere.
bool AytherSession::set_audio_event_match_rule(uint64_t signature,
                                               AudioMatchRule rule) {
    Impl& im = *impl_;
    if (rule == AudioMatchRule::kExact) {
        // : apagar la regla NO tira el timbre. La entrada queda con
        // kExact — el índice no la indexa (AudioMatchIndex ignora las
        // exactas) y el writer no la escribe (formato legacy intacto), pero
        // volver a `instrument` no exige que el sonido vuelva a sonar. Sin
        // esto, `exact` era un viaje de ida: el timbre persistido en el TOML
        // no se releía y la autoría se perdía por un toggle.
        if (const auto it = im.audio_event_rule.find(signature);
            it != im.audio_event_rule.end())
            it->second.rule = AudioMatchRule::kExact;
        im.rebuild_match_index();
        return true;
    }
    if (!im.audio_event_assign.count(signature)) return false;
    uint64_t instr = 0;
    uint8_t  pitch = kAudioNoPitch;
    if (!audio_signature_identity(signature, &instr, &pitch)) return false;
    if (rule == AudioMatchRule::kInstrumentPitch && pitch == kAudioNoPitch)
        return false;
    im.audio_event_rule[signature] = AudioMatchRuleInfo{rule, instr, pitch};
    im.rebuild_match_index();
    return true;
}

const std::vector<AytherSession::PackOverlay>&
AytherSession::pack_overlays() const noexcept {
    return impl_->overlays;
}

bool AytherSession::has_ayther_abi() const noexcept {
    return impl_->runner.has_ayther_v1();
}

uint32_t AytherSession::ayther_abi_version() const noexcept {
    return impl_->runner.ayther_abi_version();
}

const char* AytherSession::ayther_build_id() const noexcept {
    const Impl& im = *impl_;
    if (!im.runner.has_ayther_v1()) return "";
    const ayther_interface_v1* api = im.runner.ayther_api();
    // `build_id` viaja con su tamaño y NO promete terminador: se copia una vez
    // a un buffer propio para poder devolver un C-string.
    static thread_local std::string cached;
    cached.assign(api->build_id ? api->build_id : "",
                  api->build_id ? api->build_id_size : 0u);
    return cached.c_str();
}

AytherSession::SystemInfo AytherSession::system_info() const noexcept {
    const Impl& im = *impl_;
    SystemInfo o;
    if (!im.sys_ok) return o;
    o.ok = true;
    o.system_hw = im.sys.system_hw;   o.region_pal = im.sys.region_pal;
    o.vdp_mode  = im.sys.vdp_mode;    o.interlace  = im.sys.interlace;
    o.h40       = im.sys.h40;         o.shadow_highlight = im.sys.shadow_highlight;
    o.lines_per_frame = im.sys.lines_per_frame;
    o.viewport_x = im.sys.viewport_x; o.viewport_y = im.sys.viewport_y;
    o.viewport_w = im.sys.viewport_w; o.viewport_h = im.sys.viewport_h;
    o.geometry_pending = (im.sys.flags & AYTHER_SYSTEM_GEOMETRY_PENDING) != 0;
    return o;
}

void AytherSession::ayther_subscriptions(uint32_t* requested, uint32_t* active,
                                         uint32_t* supported) const noexcept {
    const Impl& im = *impl_;
    if (requested) *requested = im.ayther_subs_requested;
    if (active)    *active    = 0;
    if (supported) *supported = 0;
    if (!im.runner.has_ayther_v1()) return;
    const ayther_interface_v1* api = im.runner.ayther_api();
    if (!(api->capabilities & AYTHER_CAP_SUBSCRIPTIONS_V1)) return;
    ayther_subscription_state_v1 st{};
    st.struct_size = sizeof(st);
    if (api->get_subscriptions(&st, sizeof(st)) != AYTHER_STATUS_OK) return;
    if (active)    *active    = st.active_mask;
    if (supported) *supported = st.supported_mask;
}

bool AytherSession::audio_signature_identity(uint64_t signature,
                                             uint64_t* instrument,
                                             uint8_t* pitch) const noexcept {
    const Impl& im = *impl_;
    for (const auto& e : im.audio_events)
        if (e.signature == signature && e.instrument) {
            if (instrument) *instrument = e.instrument;
            if (pitch) *pitch = e.pitch;
            return true;
        }
    if (const auto li = im.live_sig_instr.find(signature);
        li != im.live_sig_instr.end() && li->second.instrument) {
        if (instrument) *instrument = li->second.instrument;
        if (pitch) *pitch = li->second.pitch;
        return true;
    }
    // : tercera fuente — la identidad que YA autoró el proyecto. Llega acá
    // cuando la toma no está cargada y el sonido todavía no sonó en vivo: el
    // dato existe (vino del TOML o de una regla puesta antes en esta sesión) y
    // rendirse obligaba a esperar a que el pasaje volviera a sonar.
    if (const auto r = im.audio_event_rule.find(signature);
        r != im.audio_event_rule.end() && r->second.instrument) {
        if (instrument) *instrument = r->second.instrument;
        if (pitch) *pitch = r->second.pitch;
        return true;
    }
    return false;
}

AudioMatchRule AytherSession::audio_event_match_rule(
    uint64_t signature, uint64_t* instrument, uint8_t* pitch) const noexcept {
    const auto it = impl_->audio_event_rule.find(signature);
    if (it == impl_->audio_event_rule.end()) {
        if (instrument) *instrument = 0;
        if (pitch) *pitch = kAudioNoPitch;
        return AudioMatchRule::kExact;
    }
    if (instrument) *instrument = it->second.instrument;
    if (pitch) *pitch = it->second.pitch;
    return it->second.rule;
}
uint32_t AytherSession::audio_event_assignment_count() const noexcept {
    return static_cast<uint32_t>(impl_->audio_event_assign.size());
}
std::string AytherSession::audio_event_asset(uint64_t signature) const {
    const auto it = impl_->audio_event_assign.find(signature);
    return it == impl_->audio_event_assign.end() ? std::string() : it->second;
}
void AytherSession::set_audio_substitution_preview(bool on) noexcept {
    Impl& im = *impl_;
    // Al APAGAR (toggle Assets / salir del workspace): cortar también los HD
    // que ya están sonando — sin esto un one-shot largo (p.ej. un aliento de
    // 5s) seguía en el aire con la sustitución ya deshabilitada.
    if (im.audio_sub_preview && !on) im.audio.stop_all_sfx();
    im.audio_sub_preview = on;
}

void AytherSession::set_transport_playing(bool playing) noexcept {
    Impl& im = *impl_;
    // PAUSA (transición → false): corte TOTAL del gameplay () — no sólo
    // los HD en el aire (reporte 2026-07-23) sino también el PCM del
    // original/router/SF2 ya encolado en los streams continuos: el colchón
    // DRC (~70 ms) seguía sonando tras el botón. Los previews de autoría
    // (streams marcados preview) no son del transporte y no se cortan.
    if (im.transport_playing && !playing && im.audio_enabled)
        im.audio.cut_transport_audio();
    // REANUDAR (transición → true): re-armar los disparadores — el scrub en
    // pausa los marcó «sin sonar»; al reanudar dentro de una ventana el HD
    // debe arrancar (con offset) en vez de quedarse mudo (reporte 2026-07-23).
    if (!im.transport_playing && playing) {
        im.audio_seq_fired.clear();
        im.audio_evt_fired.clear();
        // : los reemplazos LIVE no esperan un key-on nuevo — sus
        // instancias lógicas sobrevivieron al corte () y se re-arman
        // desde el offset del reloj emulado. audio_live_prev NO se limpia:
        // re-disparar desde cero corrige el silencio pero mete desfase.
        im.transport_playing = true;   // resume_live_instances lo consulta
        im.resume_live_instances();
    }
    im.transport_playing = playing;
}

void AytherSession::audio_gate_counts(uint64_t* flushed, uint64_t* inaudible,
                                      uint64_t* quiet, uint64_t* disabled) const noexcept {
    const Impl& im = *impl_;
    if (flushed)   *flushed   = im.aud_n_flushed;
    if (inaudible) *inaudible = im.aud_n_inaudible;
    if (quiet)     *quiet     = im.aud_n_quiet;
    if (disabled)  *disabled  = im.aud_n_disabled;
}

void AytherSession::audio_pause_stats(uint64_t* cut_frames,
                                      uint64_t* cuts) const noexcept {
    if (cut_frames) *cut_frames = impl_->audio.pause_cut_frames();
    if (cuts)       *cuts       = impl_->audio.pause_cuts();
}

void AytherSession::synth_stats(uint64_t* ticks, uint64_t* jumps,
                                uint64_t* note_on, uint64_t* note_off,
                                uint64_t* muted, uint64_t* no_pcm) const noexcept {
    const Impl& im = *impl_;
    if (ticks)    *ticks    = im.syn_ticks;
    if (jumps)    *jumps    = im.syn_jumps;
    if (note_on)  *note_on  = im.syn_on;
    if (note_off) *note_off = im.syn_off;
    if (muted)    *muted    = im.syn_muted;
    if (no_pcm)   *no_pcm   = im.syn_nopcm;
}

const std::vector<AytherSession::AudioSeqSub>& AytherSession::audio_seq_subs() const noexcept {
    return impl_->audio_seq_subs;
}

std::vector<uint32_t> AytherSession::audio_seq_anchors(uint64_t key) {
    Impl& im = *impl_;
    for (const auto& sq : im.audio_seq_subs)
        if (sq.key == key) return im.seq_anchors_of(sq);
    return {};
}

void AytherSession::audio_mute_stats(uint64_t* hd_muted,
                                     uint64_t* hd_cut) const noexcept {
    const Impl& im = *impl_;
    if (hd_muted) *hd_muted = im.hd_muted;
    if (hd_cut)   *hd_cut   = im.hd_cut;
}

void AytherSession::audio_fallback_stats(uint64_t* fallbacks,
                                         uint64_t* start_fails) const noexcept {
    const Impl& im = *impl_;
    if (fallbacks)   *fallbacks   = im.hd_fallback;
    if (start_fails) *start_fails = im.audio.hd_start_fails();
}

const char* AytherSession::audio_asset_error(const char* asset_path) const {
    if (!asset_path || !asset_path[0]) return nullptr;
    return impl_->audio.asset_error_name(asset_path);
}

size_t AytherSession::audio_sfx_count() const noexcept {
    return impl_->audio.sfx_count();
}

bool AytherSession::audio_audible() const noexcept { return impl_->audio_audible; }

void AytherSession::set_audio_audible(bool on) noexcept {
    impl_->audio_audible = on;
}

void AytherSession::set_audio_runtime_substitution(bool on) noexcept {
    Impl& im = *impl_;
    if (on == im.audio_runtime_sub) return;
    im.audio_runtime_sub  = on;
    im.audio_runtime_mask = 0;
    im.audio_live_prev.clear();
    im.live_active.clear();
    //  F2: el detector NO se resetea acá — come siempre en el produce y
    // sigue al runner, así que al volver a Capturar las voces sostenidas ya
    // están abiertas con su firma correcta (antes: reset ciego → hasta que el
    // juego reescribiera patch/key-on no se reconocía nada). El reset vive en
    // las transiciones reales de sesión (reset()/ROM). `audio_live_prev`
    // recién limpiado = las voces vigentes entran como flanco de subida en el
    // primer frame — una disparadora sostenida ancla su Secuencia al entrar.
    // Ventanas de Secuencia en vivo: cortar los HD en el aire y limpiar el
    // estado de anclaje en ambos sentidos (salir de Capturar no debe dejar un
    // one-shot sonando ni un next_ok viejo bloqueando el próximo encendido).
    if (im.audio_enabled)
        for (const auto& w : im.audio_live_seq_win) im.audio.stop_sfx_by_key(w.key);
    im.audio_live_seq_win.clear();
    im.audio_live_seq_next.clear();
    // : cambio de workspace = cierre EXPLÍCITO de las instancias live —
    // sus streams también (los event-streams del pack quedaban drenando tras
    // salir de Capturar). Las ventanas de evento son estado live y se van
    // con ellas.
    if (im.audio_enabled)
        for (const auto& kv : im.audio_live_inst) {
            im.audio.stop_sfx_by_key(kv.first);
            im.audio.stop_event(kv.first);
        }
    im.audio_live_inst.clear();
    im.audio_seq_windows.clear();
    //  F4: el registro de sin-match referencia frames de ESTA estadía en
    // el workspace; lo aprendido (sig→instrument) sí sobrevive — la
    // identidad no depende del workspace.
    im.live_unmatched.clear();
    if (!on) im.runner.set_audio_mute_v1(0);   // restaurar al apagar
}

void AytherSession::set_audio_live_bypass(bool bypass) noexcept {
    Impl& im = *impl_;
    if (bypass == im.audio_live_bypass) return;
    im.audio_live_bypass = bypass;
    if (bypass) {
        // Assets OFF ( Fase 3): detener los streams — el bookkeeping
        // (detector, ventanas, instancias, flancos) sigue corriendo para no
        // perder el hilo; la máscara cae a 0 el próximo frame y suena el
        // juego original.
        if (im.audio_enabled)
            for (const auto& kv : im.audio_live_inst) {
                im.audio.stop_sfx_by_key(kv.first);
                im.audio.stop_event(kv.first);
            }
        im.audio_runtime_mask = 0;
    } else {
        // Assets ON dentro de un evento: mismo camino que reanudar una
        // pausa — entra con el offset del reloj emulado, sin key-on nuevo.
        im.resume_live_instances();
    }
}

void AytherSession::audio_resume_stats(uint64_t* resumed, uint64_t* finished,
                                       uint64_t* offset_frames) const noexcept {
    const Impl& im = *impl_;
    if (resumed)       *resumed       = im.hd_resumed;
    if (finished)      *finished      = im.hd_resume_finished;
    if (offset_frames) *offset_frames = im.hd_resume_offset_frames;
}

void AytherSession::audio_unified_stats(uint64_t* voices, uint64_t* started,
                                        uint64_t* skew,
                                        uint64_t* max_skew) const noexcept {
    const Impl& im = *impl_;
    if (voices)   *voices   = im.audio.hd_voice_count();
    if (started)  *started  = im.audio.hd_voices_started();
    if (skew)     *skew     = im.audio.hd_mix_skew();
    if (max_skew) *max_skew = im.audio.hd_mix_max_skew();
}

void AytherSession::audio_live_match_stats(uint64_t* exact, uint64_t* rule,
                                           uint64_t* variant,
                                           uint64_t* unmatched) const noexcept {
    const Impl& im = *impl_;
    if (exact)     *exact     = im.live_match_exact;
    if (rule)      *rule      = im.live_match_rule;
    if (variant)   *variant   = im.live_match_variant;
    if (unmatched) *unmatched = im.live_match_none;
}

void AytherSession::audio_live_match_reset() noexcept {
    Impl& im = *impl_;
    im.live_unmatched.clear();
    im.live_match_exact = im.live_match_rule = 0;
    im.live_match_variant = im.live_match_none = 0;
}

size_t AytherSession::audio_live_unmatched(AudioLiveUnmatched* out,
                                           size_t cap) const {
    const Impl& im = *impl_;
    size_t n = 0;
    for (const auto& [sig, rec] : im.live_unmatched) {
        if (out && n < cap)
            out[n] = AudioLiveUnmatched{ sig, rec.instrument, rec.first_frame,
                                         rec.frames_active, rec.chip,
                                         rec.channel, rec.variant };
        ++n;
    }
    return n;   // total (puede superar cap — el caller dimensiona)
}

uint32_t AytherSession::audio_live_active(AytherAudioActive* out, uint32_t cap) const {
    if (!impl_->audio_live_det || !out) return 0;
    return ayther_audio_event_active(impl_->audio_live_det.get(), out, cap);
}

void AytherSession::set_audio_manual_mute(uint32_t mask) noexcept {
    impl_->audio_manual_mute = mask;
}

void AytherSession::set_audio_occurrence_mute(const uint64_t* keys, uint32_t n) noexcept {
    impl_->audio_occurrence_mute.clear();
    if (keys)
        for (uint32_t i = 0; i < n; ++i) impl_->audio_occurrence_mute.insert(keys[i]);
}

void AytherSession::set_instrument_assigns(const InstrumentAssign* a, uint32_t n) {
    Impl& im = *impl_;

    // ESTO LLEGA CADA FRAME. El frontend manda su catálogo entero en cada vuelta
    // (fuente única: la UI y MCP escriben ahí), así que reconstruir siempre era
    // catastrófico: synth_panic() apaga TODAS las notas en vuelo, o sea que el
    // timbre arrancaba y se cortaba 60 veces por segundo — «casi ni se escucha,
    // parece comenzar a reproducirse pero casi de inmediato se corta» (reporte
    // 2026-07-28). Y de paso recargaba el SoundFont, que era la ralentización.
    //
    // Los contadores lo decían sin que yo los leyera bien: 58 note_on y CERO
    // note_off. No es que las notas no se cerraran — es que las mataba el panic,
    // que no cuenta note_offs.
    //
    // Sólo la ESTRUCTURA (qué timbre, qué archivo, qué preset, qué transposición)
    // obliga a reconstruir. La GANANCIA se actualiza en caliente: tiene que oírse
    // mientras se mueve el slider (), y matar las notas para aplicarla sería
    // absurdo — era también por qué «subir la ganancia no hacía cambio».
    if (n == 0 && im.inst_assign.empty()) return;
    bool same_shape = a && n == im.inst_assign.size();
    for (uint32_t i = 0; same_shape && i < n; ++i) {
        const auto it = im.inst_assign.find(a[i].patch);
        same_shape = it != im.inst_assign.end()
            && it->second.soundfont == (a[i].soundfont ? a[i].soundfont : "")
            && it->second.bank      == a[i].bank
            && it->second.preset    == a[i].preset
            && it->second.transpose == a[i].transpose;
    }
    if (same_shape) {
        for (uint32_t i = 0; i < n; ++i) {
            const float g = a[i].gain > 0.0f ? a[i].gain : 1.0f;
            im.inst_assign[a[i].patch].gain = g;
            if (g > 1.0f) im.synth_boost[a[i].patch] = g;
            else          im.synth_boost.erase(a[i].patch);
        }
        return;
    }

    im.synth_panic();
    for (auto& [_, sy] : im.synths) ayther_sf2_free(sy);
    im.synths.clear();
    im.inst_assign.clear();
    im.synth_any = false;
    if (!a || n == 0) return;

    for (uint32_t i = 0; i < n; ++i) {
        if (!a[i].patch || !a[i].soundfont || !*a[i].soundfont) continue;
        const std::string sf = a[i].soundfont;

        // Lazy-load por TIMBRE: cada uno necesita su propio sintetizador para
        // poder realzar su ganancia (). El SoundFont parseado se comparte
        // del lado Rust por `key`, así que N timbres del mismo archivo no lo
        // duplican — con un SF2 de 988 MB eso no sería opcional. La carga
        // (pack primero, disco después) vive en load_sf2_shared, compartida
        // con el render offline del export ().
        if (!im.synths.count(a[i].patch)) {
            AytherSf2* sy = im.load_sf2_shared(sf);
            if (!sy)
                std::fprintf(stderr, "[sf2] no se pudo cargar '%s' (ni del pack "
                             "ni de disco) — ese timbre suena con su chip\n",
                             sf.c_str());
            // Se cachea AUNQUE sea nulo: sin esto se reintentaría (y se
            // loguearía) por cada asignación que lo referencie.
            im.synths[a[i].patch] = sy;
        }

        Impl::InstAssign as;
        as.soundfont = sf;
        as.bank = a[i].bank;
        as.preset = a[i].preset;
        as.transpose = a[i].transpose;
        as.gain = a[i].gain > 0.0f ? a[i].gain : 1.0f;
        im.inst_assign[a[i].patch] = as;
        im.synth_any = true;
    }

    // Preset y REALCE por timbre. El realce (>1) se guarda acá y se aplica al
    // render de su sintetizador; la atenuación (<1) va por CC 7 en el note_on,
    // que es donde el artista puede moverla sin re-asignar.
    im.synth_boost.clear();
    for (auto& [inst, as] : im.inst_assign) {
        if (AytherSf2* sy = im.synth_for(inst)) {
            ayther_sf2_program(sy, 0, as.preset);
            if (as.gain > 1.0f) im.synth_boost[inst] = as.gain;
        }
    }
    // Los SoundFonts que ya no usa nadie se sueltan: sin esto, cambiar de
    // archivo dejaría el viejo cargado.
    ayther_sf2_trim_cache();
}

void AytherSession::set_widescreen(uint32_t logical_w) noexcept {
    impl_->wide_w = logical_w;
}
uint32_t AytherSession::widescreen() const noexcept { return impl_->wide_w; }

void AytherSession::set_widescreen_gate(const std::string& toml) {
    // Texto vacío o sin `[[widescreen]]` DESARMA el gate: el core devuelve NULL
    // y el ancho pedido vuelve a mandar. Apagarlo no es lo mismo que fijarlo
    // en 4:3 — un pack sin declaración no puede desactivar el ensanchado que el
    // Lab tiene puesto a mano.
    impl_->wide_gate.reset(toml.empty() ? nullptr
                                        : ayther_widescreen_gate_new(toml.c_str()));
}
bool AytherSession::widescreen_gated() const noexcept {
    return impl_->wide_gate != nullptr;
}
uint32_t AytherSession::widescreen_effective() const noexcept {
    return impl_->wide_w_eff;
}

void AytherSession::synth_panic() noexcept { impl_->synth_panic(); }

void AytherSession::set_voice_router(bool on) noexcept {
    Impl& im = *impl_;
    if (im.voice_router_on == on) return;
    im.voice_router_on = on;
    im.voice_pending.clear();
    im.voice_router.reset();
    im.voice_rs.reset();
    im.voice_last_frame = -1;
    if (on) {
        im.voice_policy.im = &im;
        // : una fuente con ganancia por bus, apuntando al array de la
        // sesión. Se arman acá y no en el constructor porque necesitan la
        // dirección de `im.bus_gain`, que existe recién con la sesión hecha.
        im.voice_policy.gains.clear();
        im.voice_policy.gains.reserve(kAudioBusCount);
        for (uint32_t b = 0; b < kAudioBusCount; ++b)
            im.voice_policy.gains.emplace_back(&im.bus_gain[b]);
        im.voice_router.set_policy(&im.voice_policy);
        // La tasa del espejo es la NATIVA del YM2612 (MCLK/1008); el device
        // corre a otra, así que el resampler es obligatorio y su fase se
        // conserva entre bloques (el catch-up entrega bloques de tamaño
        // distinto, y reiniciar la fase metería un chasquido por frame).
        const double dst = 44100.0;   // tasa del stream del sintetizador
        const double fps = im.runner.fps();
        im.voice_router.mirror().set_pal(fps > 1.0 && fps < 55.0);
        im.voice_rs.set_rates(im.voice_router.mirror().rate(), dst);
        // Cebar hasta donde está el cabezal: si no, el espejo arranca en frío y
        // el timbre sale mal hasta que el juego reescriba los patches.
        im.voice_prime_to(static_cast<uint32_t>(im.frame_index));
    } else {
        // Apagarlo restituye el camino viejo: el mute vuelve a armarse por
        // ventanas en el próximo produce.
        im.audio.clear_synth();
    }
    std::fprintf(stdout, "[voice] router de canales por voz: %s\n", on ? "PUESTO" : "sacado");
}

bool AytherSession::voice_router() const noexcept { return impl_->voice_router_on; }

void AytherSession::voice_router_stats(uint64_t* ticks, uint64_t* chip_frames,
                                       uint64_t* primes, uint64_t* starved,
                                       uint64_t* substituted) const noexcept {
    const Impl& im = *impl_;
    if (ticks)       *ticks       = im.vr_ticks;
    if (chip_frames) *chip_frames = im.vr_frames;
    if (primes)      *primes      = im.vr_primes;
    if (starved)     *starved     = im.vr_starved;
    if (substituted) *substituted = im.voice_router.stats().substituted;
}

void AytherSession::set_audio_instrument_mute(const uint64_t* instruments, uint32_t n) noexcept {
    impl_->audio_instrument_mute.clear();
    if (instruments)
        for (uint32_t i = 0; i < n; ++i) impl_->audio_instrument_mute.insert(instruments[i]);
}

void AytherSession::set_audio_sequence_subs(std::vector<AudioSeqSub> subs) {
    Impl& im = *impl_;
    // Prewarm de los assets nuevos (el set se re-manda cada frame; el
    // unordered_set dedupea → decode una sola vez, en el primer frame tras
    // abrir el proyecto / asignar el HD, no en pleno playback).
    for (const auto& s : subs) {
        if (!s.asset.empty() && im.audio_prewarmed.insert(s.asset).second)
            im.audio.prewarm_asset_file(s.asset);
        // : cambiar el ASSET de una Secuencia re-arma su transacción — un
        // fallo de arranque viejo no sobrevive al reemplazo. Solo al cambiar:
        // el set se re-manda cada frame y borrar siempre anularía el registro.
        for (const auto& old : im.audio_seq_subs)
            if (old.key == s.key) {
                if (old.asset != s.asset) im.hd_failed_keys.erase(s.key);
                break;
            }
    }
    // Subs que DESAPARECEN (Secuencia deshabilitada con el ojo / HD quitado):
    // cortar su one-shot en el aire y re-armar el disparador — el chequeo de
    // ventana por frame ya no las ve, así que sin esto el HD ya disparado
    // seguía sonando (reporte 2026-07-23).
    for (const auto& old : im.audio_seq_subs) {
        bool still = false;
        for (const auto& n : subs)
            if (n.key == old.key) { still = true; break; }
        if (!still) {
            if (im.audio_enabled) im.audio.stop_sfx_by_key(old.key);
            im.audio_seq_fired[old.key] = 0;
        }
    }
    // /: la política de fin autorada en la Secuencia alimenta los
    // mismos mapas por firma que usa el TOML del pack, así el disparo por
    // Secuencia y el horneado leen UNA sola fuente.
    for (const auto& s : subs) {
        if (!s.trigger_signature) continue;
        if (s.tail_frames != UINT32_MAX)
            im.audio_event_tail[s.trigger_signature] = s.tail_frames;
        if (s.fade_frames) im.audio_event_fade[s.trigger_signature] = s.fade_frames;
        else               im.audio_event_fade.erase(s.trigger_signature);
    }
    // : la tabla de anclas se invalida sólo si el set CAMBIÓ de verdad
    // (se re-manda cada frame).
    auto same = [](const AudioSeqSub& a, const AudioSeqSub& b) {
        return a.key == b.key && a.trigger_signature == b.trigger_signature &&
               a.duration_frames == b.duration_frames && a.span_frames == b.span_frames &&
               a.asset == b.asset && a.signatures == b.signatures &&
               a.head_signatures == b.head_signatures && a.looping == b.looping;
    };
    bool changed = subs.size() != im.audio_seq_subs.size();
    for (size_t i = 0; !changed && i < subs.size(); ++i)
        changed = !same(subs[i], im.audio_seq_subs[i]);
    if (changed) ++im.audio_seq_subs_gen;
    im.audio_seq_subs = std::move(subs);
}

void AytherSession::preview_asset_file(const char* path, float gain) {
    // preview=true (): es un pedido explícito de autoría — la pausa del
    // transporte del gameplay no debe cortarlo.
    if (path && path[0] && impl_->audio_enabled)
        impl_->audio.play_oneshot_asset_file(path, 0x53455130ull /* "SEQ0" */,
                                             0.0, gain, /*preview=*/true);
}

uint32_t AytherSession::audio_asset_frames(const char* path) const {
    if (!path || !path[0]) return 0;
    const double secs = impl_->audio.asset_duration_seconds(path);
    if (secs <= 0.0) return 0;
    const double fps = timing_fps() > 1.0 ? timing_fps() : 60.0;
    return static_cast<uint32_t>(secs * fps + 0.5);
}

// : el nivel medido de un asset. `const` de cara al caller aunque adentro
// mida y cachee — la sesión no cambia por preguntar, y forzar al Lab a tener
// una referencia mutable para leer un dato lo habría contagiado a media UI.
const AytherSession::AssetLevel&
AytherSession::audio_asset_level(const std::string& abs_path) const {
    return const_cast<Impl&>(*impl_).audio.asset_level(abs_path);
}

const std::vector<float>&
AytherSession::audio_asset_waveform(const std::string& abs_path,
                                    uint32_t bins) const {
    return const_cast<Impl&>(*impl_).audio.asset_waveform(abs_path, bins);
}

std::string AytherSession::audio_events_toml() const {
    const Impl& im = *impl_;
    // channels por firma = OR de los canales de las ocurrencias de esa firma.
    std::unordered_map<uint64_t, uint32_t> chmask;
    for (const auto& e : im.audio_events) {
        const uint32_t bit = chan_bit(e.chip, e.channel);
        chmask[e.signature] |= bit;
    }
    std::vector<AytherEventSub> subs;
    subs.reserve(im.audio_event_assign.size());
    for (const auto& [sig, asset] : im.audio_event_assign) {
        AytherEventSub s{};
        s.signature = sig;
        std::snprintf(s.asset, sizeof(s.asset), "%s", asset.c_str());
        const auto it = chmask.find(sig);
        s.channels = (it != chmask.end()) ? it->second : 0;
        s.match_pitch = kAudioNoPitch;
        if (const auto r = im.audio_event_rule.find(sig);
            r != im.audio_event_rule.end()) {   //  F3: la regla persiste
            s.match_rule       = static_cast<uint8_t>(r->second.rule);
            s.match_instrument = r->second.instrument;
            s.match_pitch      = r->second.pitch;
        }
        subs.push_back(s);
    }
    const uint32_t need = ayther_audio_events_format(subs.data(),
        static_cast<uint32_t>(subs.size()), nullptr, 0);
    std::string out(need, '\0');
    if (need)
        ayther_audio_events_format(subs.data(), static_cast<uint32_t>(subs.size()),
                                   out.data(), need + 1);
    return out;
}

void AytherSession::load_audio_events_toml(const char* text) {
    if (!text) return;
    const uint32_t n = ayther_audio_events_parse(text, nullptr, 0);
    std::vector<AytherEventSub> subs(n);
    if (n) ayther_audio_events_parse(text, subs.data(), n);
    impl_->audio_event_assign.clear();
    impl_->audio_event_channels.clear();
    impl_->audio_event_duration.clear();
    impl_->audio_event_looping.clear();
    // : members por firma → mute selectivo dentro de la ventana (el
    // parser Rust del catálogo tolera/ignora el campo; acá se lee con toml++).
    impl_->audio_event_members = parse_audio_event_members(text);
    impl_->audio_event_head    = parse_audio_event_sig_list(text, "head");   // 
    impl_->audio_event_seq_next.clear();
    // : tail por firma — ausente = ilimitado (legacy explícito).
    impl_->audio_event_tail = parse_audio_event_tails(text);
    // : fade de fin por firma — ausente = sin fade.
    impl_->audio_event_fade = parse_audio_event_fades(text);
    // : ganancia por firma — ausente = 1.0 (neutro).
    impl_->audio_event_gain = parse_audio_event_gains(text);
    // : region de loop por firma — ausente = el asset entero.
    impl_->audio_event_loop = parse_audio_event_loops(text);
    // : y el gate de condiciones. Lo compila el CORE (camino A): tener un
    // segundo evaluador del dialecto de condiciones es la clase de duplicacion
    // que ya se pago cara con los tres consumidores del addr de audio.
    impl_->audio_gate.reset(ayther_audio_gate_new(text));
    impl_->audio_gate_blocked.clear();
    impl_->audio_event_rule.clear();   //  F3: reglas del TOML entrante
    impl_->audio_event_bus.clear();    // : buses del TOML entrante
    for (const auto& s : subs) {
        impl_->audio_event_assign[s.signature]   = s.asset;
        impl_->audio_event_channels[s.signature] = s.channels;
        if (s.duration_frames) {   // entrada de SECUENCIA (Mezclar)
            impl_->audio_event_duration[s.signature] = s.duration_frames;
            impl_->audio_event_looping[s.signature]  = s.looping != 0;
        }
        //  F3: regla persistida con su identidad (el parser ya validó que
        // una regla sin instrumento cae a exacta). Legacy = sin regla.
        if (s.match_rule && s.match_instrument)
            impl_->audio_event_rule[s.signature] = AudioMatchRuleInfo{
                static_cast<AudioMatchRule>(s.match_rule),
                s.match_instrument, s.match_pitch};
        // : el bus del pack. Sólo si lo declaró — 0 es «no lo dijo», y
        // anotarlo haría que una firma sin clasificar dejara de caer al
        // default, que es una decisión distinta.
        if (s.bus)
            impl_->audio_event_bus[s.signature] =
                static_cast<AudioBus>(s.bus % kAudioBusCount);
        // Prewarm al ABRIR el proyecto (este load corre ahí): decodificar ya,
        // no en el primer disparo en pleno playback (stall → trigger salteado).
        if (s.asset[0] &&
            impl_->audio_prewarmed.insert(s.asset).second)
            impl_->audio.prewarm_asset_file(s.asset);
    }
    impl_->rebuild_match_index();
}

void AytherSession::load_audio_events_from_pack() {
    Impl& im = *impl_;
    if (!im.pack) return;
    AyArchive* p = im.pack.get();
    const int64_t sz = ayther_pack_file_size(p, "audio_events.toml");
    if (sz <= 0) return;
    std::vector<uint8_t> raw(static_cast<size_t>(sz) + 1, 0);   // +1 para el nul
    if (ayther_pack_read(p, "audio_events.toml", raw.data(), static_cast<size_t>(sz)) <= 0)
        return;
    load_audio_events_toml(reinterpret_cast<const char*>(raw.data()));
}

// Limpia el cache de replay (keyframes + cursor). El caller lo invoca cuando el
// objeto AytherRecording se reusa con OTRO contenido (cargar otra toma, split):
// el puntero no cambia, así que el chequeo de identidad de replay_seek no basta.
void AytherSession::replay_reset() {
    impl_->replay_rec = nullptr;
    impl_->replay_pos = -1;
    impl_->replay_keys.clear();
    impl_->chunk.active = false;
    impl_->bake.active  = false;
    if (impl_->tween) ayther_tween_clear(impl_->tween.get());   // otra toma: sin tweens arrastrados
    impl_->audio_seq_windows.clear();                           // ventanas de secuencia fuera
    impl_->audio_seq_fired.clear();                             // disparos hechos fuera también
    impl_->audio_evt_fired.clear();
    if (impl_->audio_enabled) impl_->audio.stop_all_events();   // otra toma: streams fuera
}

void AytherSession::replay_invalidate() { impl_->replay_pos = -1; }
size_t AytherSession::replay_key_count() const { return impl_->replay_keys.size(); }

// Frames que un replay_seek(rec, frame) tendría que re-simular (distancia al
// mejor arranque). El frontend decide con esto: seek directo si es barato, o
// seek en chunks con loader si es caro (toma fría, sin keyframe cercano).
uint32_t AytherSession::replay_seek_cost(const AytherRecording& rec,
                                         uint32_t frame) const {
    const Impl& im = *impl_;
    if (rec.empty()) return 0;
    const uint32_t target = frame < rec.frame_count() ? frame : rec.frame_count() - 1;
    if (im.replay_rec == &rec) {
        if (im.replay_pos == static_cast<int>(target)) return 0;       // ya posicionado
        if (im.replay_pos >= 0 &&
            target == static_cast<uint32_t>(im.replay_pos) + 1) return 0;  // fast +1
    }
    const uint32_t start = replay_start_frame(rec, target);   // runtime u horneado
    return target > start ? target - start : 0;
}

// Seek en chunks: re-simula a lo sumo `budget` frames bare por llamada hacia
// `frame`, dejando la máquina a media cadena entre llamadas. Es exactamente el
// camino general de replay_seek (unserialize del keyframe más cercano + bare +
// produce del visible), troceado: mismo resultado, sin congelar la UI.
AytherSession::SeekStep AytherSession::replay_seek_chunk(const AytherRecording& rec,
                                                         uint32_t frame, uint32_t budget) {
    Impl& im = *impl_;
    SeekStep r{};
    if (rec.empty()) { r.done = true; return r; }
    const uint32_t target = frame < rec.frame_count() ? frame : rec.frame_count() - 1;
    std::fprintf(stderr, "[dbgsk] seek_chunk f=%u pos=%d activo=%d\n",
                 frame, im.replay_pos, (int)im.chunk.active);

    // (Re)inicia si no hay chunk activo o cambió el objetivo/grabación.
    if (!im.chunk.active || im.chunk.rec != &rec || im.chunk.target != target) {
        const std::vector<uint8_t>* state = nullptr;
        const uint32_t startF = replay_start(rec, target, state);   // runtime u horneado
        if (target <= startF) {                 // nada que re-simular → directo
            r.view = replay_seek(rec, target);  // (también resetea chunk.active)
            r.done = true;
            return r;
        }
        if (!im.runner.unserialize(*state)) { r.done = true; return r; }
        im.replay_rec = &rec;
        im.replay_pos = -1;                     // máquina a media cadena bare
        im.chunk = { &rec, true, target, startF, startF };
    }

    uint32_t did = 0;
    while (im.chunk.cur < target && did < budget) {
        im.runner.set_input(0, rec.inputs[im.chunk.cur]);
        im.runner.run_frame();                  // bare, silencioso
        ++im.chunk.cur;
        ++did;
    }
    if (im.audio_enabled) im.audio.discard_emulator();   // descarta el PCM del re-sim

    if (im.chunk.cur >= target) {
        im.runner.set_input(0, rec.inputs[target]);
        im.frame_index = target;
        r.view = &produce_frame();              // frame visible (con su audio)
        im.replay_pos = static_cast<int>(target);
        replay_capture_key(target + 1);
        im.chunk.active = false;
        r.done = true;
    } else {
        const uint32_t span = target - im.chunk.start;
        r.progress = span ? static_cast<float>(im.chunk.cur - im.chunk.start) / span : 1.0f;
    }
    return r;
}

// Migración R7e: hornea keyframes en una toma vieja (sin keyframes), troceado
// para no congelar. Bombear hasta done; al terminar, rec.keyframes está poblado
// y el caller re-guarda el .arp. Warmea recorriendo las fronteras con replay_seek
// (cada una captura su keyframe runtime) y al final los comprime en la toma.
AytherSession::SeekStep AytherSession::replay_bake_step(AytherRecording& rec, uint32_t budget) {
    Impl& im = *impl_;
    SeekStep r{};
    if (rec.empty() || !rec.keyframes.empty() ||
        rec.frame_count() <= kReplayKeyInterval) { r.done = true; return r; }
    const uint32_t last = rec.frame_count() - 1;

    if (!im.bake.active || im.bake.rec != &rec) {
        im.replay_rec  = &rec;
        im.replay_keys.clear();
        im.replay_pos  = -1;
        im.bake = { &rec, true, 0 };
    }

    im.replay_quiet = true;                 // el warm no debe sonar
    uint32_t did = 0;
    while (im.bake.cur < last && did < budget) {
        const uint32_t boundary = ((im.bake.cur / kReplayKeyInterval) + 1) * kReplayKeyInterval - 1;
        const uint32_t next = boundary < last ? boundary : last;
        replay_seek(rec, next);             // captura keys[next+1] (frontera)
        did += (next > im.bake.cur) ? (next - im.bake.cur) : kReplayKeyInterval;
        im.bake.cur = next + 1;
    }
    im.replay_quiet = false;

    if (im.bake.cur >= last) {
        for (auto& [frame, st] : im.replay_keys) rec.add_keyframe(frame, st);
        im.replay_keys.clear();             // ya horneados → liberar RAM runtime
        im.bake.active = false;
        r.done = true;
    } else {
        r.progress = last ? static_cast<float>(im.bake.cur) / last : 1.0f;
    }
    return r;
}

// Migración v8: re-hornea la HISTORIA de hashes de sprites con el hasher vigente
// (rec.hash_algo < kSpriteHashAlgo → la historia se capturó con OTRA función y
// present()/marks no encuentran las poses). Recorre la toma produciendo CADA
// frame (secuencial = produce encadenado, el camino rápido de replay_seek) y
// reconstruye el CSR desde las occurrences vivas. Máscaras/supresiones/overrides
// del Lab se stashean por chunk: la historia debe registrar el juego COMPLETO
// (una supresión activa sacaría sprites del parseo del VDP). Al terminar,
// también hornea keyframes si faltaban (el barrido los capturó gratis).
AytherSession::SeekStep AytherSession::replay_rebake_history_step(
        AytherRecording& rec, uint32_t budget) {
    Impl& im = *impl_;
    SeekStep r{};
    const uint32_t n = rec.frame_count();
    if (rec.empty()) { r.done = true; return r; }

    if (!im.hbake.active || im.hbake.rec != &rec) {
        im.replay_rec = &rec;
        im.replay_keys.clear();
        im.replay_pos = -1;
        im.hbake = {};
        im.hbake.rec    = &rec;
        im.hbake.active = true;
        im.hbake.off.assign(1, 0);
        im.hbake.hashes.reserve(rec.sprite_hashes.size());
    }

    // Stash del contexto de autoría del Lab (se restaura al final del chunk; la
    // app no produce entre chunks — muestra el loader de migración).
    const auto sv_overrides = std::move(im.preview_pose_overrides);
    im.preview_pose_overrides.clear();
    const bool sv_sup  = im.suppress_any;            im.suppress_any            = false;
    const bool sv_tile = im.tile_suppress_any;       im.tile_suppress_any       = false;
    const bool sv_pln  = im.plane_tile_suppress_any; im.plane_tile_suppress_any = false;
    const uint8_t sv_mask = im.layer_mask_want;      im.layer_mask_want         = 0xFF;
    const bool sv_dim = im.layer_dim_want;           im.layer_dim_want          = false;

    im.replay_quiet = true;
    uint32_t did = 0;
    bool failed = false;
    while (im.hbake.cur < n && did < budget) {
        const FrameView* fv = replay_seek(rec, im.hbake.cur);
        if (!fv) { failed = true; break; }
        for (uint32_t i = 0; i < fv->sprite_occ_count; ++i)
            im.hbake.hashes.push_back(fv->sprite_occs[i].hash);
        im.hbake.off.push_back(static_cast<uint32_t>(im.hbake.hashes.size()));
        ++im.hbake.cur;
        ++did;
    }
    im.replay_quiet = false;

    im.preview_pose_overrides  = std::move(sv_overrides);
    im.suppress_any            = sv_sup;
    im.tile_suppress_any       = sv_tile;
    im.plane_tile_suppress_any = sv_pln;
    im.layer_mask_want         = sv_mask;
    im.layer_dim_want          = sv_dim;

    if (failed) {                       // seek roto: abortar sin tocar la toma
        im.hbake = {};
        r.done = true;
        return r;
    }
    if (im.hbake.cur >= n) {
        rec.sprite_hashes = std::move(im.hbake.hashes);
        rec.hash_offsets  = std::move(im.hbake.off);
        rec.hash_algo     = AytherRecording::kSpriteHashAlgo;
        if (rec.keyframes.empty())
            for (auto& [frame, st] : im.replay_keys) rec.add_keyframe(frame, st);
        im.hbake = {};
        r.done = true;
    } else {
        r.progress = static_cast<float>(im.hbake.cur) / static_cast<float>(n);
    }
    return r;
}

// Fase C: split determinista. El savestate del tail se captura POST-frame
// (frame-1) llegando por el MISMO camino que usa el replay (replay_seek:
// re-sim bare + produce_frame del frame visible) — serializar tras una
// cadena de run_frame "bare" produce un estado que no reproduce (verificado
// con split_smoke: el framebuffer divergía desde el primer frame del tail).
bool AytherSession::split_recording(const AytherRecording& rec, uint32_t frame,
                                    AytherRecording& head, AytherRecording& tail) {
    Impl& im = *impl_;
    if (rec.empty() || frame == 0 || frame >= rec.frame_count()) return false;
    if (!replay_seek(rec, frame - 1)) return false;  // estado PRE-frame `frame`
    std::vector<uint8_t> st;
    if (!im.runner.serialize(st) || st.empty()) return false;
    head = rec.slice(0, frame, rec.initial_state);
    tail = rec.slice(frame, rec.frame_count(), std::move(st));
    return true;
}

// Corte destructivo: mismo contrato de savestate que split_recording (estado
// PRE-frame `begin` capturado por el camino del replay); begin==0 no re-simula.
bool AytherSession::crop_recording(const AytherRecording& rec, uint32_t begin,
                                   uint32_t end, AytherRecording& out) {
    Impl& im = *impl_;
    if (rec.empty() || begin >= end || end > rec.frame_count()) return false;
    if (begin == 0) { out = rec.slice(0, end, rec.initial_state); return true; }
    if (!replay_seek(rec, begin - 1)) return false;  // estado PRE-frame `begin`
    std::vector<uint8_t> st;
    if (!im.runner.serialize(st) || st.empty()) return false;
    out = rec.slice(begin, end, std::move(st));
    return true;
}

// ---------------------------------------------------------------------------
// Modo avanzado (Lab): Work RAM + cheats
// ---------------------------------------------------------------------------
const uint8_t* AytherSession::work_ram(size_t* size) const {
    if (size) *size = impl_->runner.work_ram_size();
    return impl_->runner.work_ram();
}

const uint8_t* AytherSession::video_ram(size_t* size) const {
    if (size) *size = impl_->runner.video_ram_size();
    return impl_->vram_ptr();                              // E-5
}

const uint8_t* AytherSession::color_ram(size_t* size) const {
    if (size) *size = impl_->runner.color_ram_size();
    return impl_->cram_ptr();                              // E-5
}

uint64_t AytherSession::palette_signature(uint8_t line, uint16_t slots) const {
    // /: firma de contenido de una línea de paleta desde la CRAM viva —
    // la MISMA fn del runtime (ayther_palette_signature) para que autoría y
    // resolución no diverjan. La captura del Lab (clonar Actor por firma) la usa.
    const uint8_t* cram = impl_->cram_ptr();               // E-5
    const size_t   csz  = impl_->runner.color_ram_size();
    if (!cram || csz < 128) return 0;
    uint16_t words[64];
    std::memcpy(words, cram, sizeof(words));
    return ayther_palette_signature(words, 64, line, slots);
}

const uint8_t* AytherSession::vdp_regs(size_t* size) const {
    if (size) *size = impl_->runner.vdp_regs_size();
    return impl_->regs_ptr();                              // E-5
}

const uint8_t* AytherSession::parsed_sprites_raw(uint8_t* count) const {
    // E-5: con ABI, el espejo que produce_frame ya leyó por read_region; sin
    // ABI, el puntero del core.
    if (impl_->abi_snap_ok && impl_->abi_sprite_count) {
        if (count) *count = static_cast<uint8_t>(
            (std::min<uint32_t>)(impl_->abi_sprite_count, 255u));
        return reinterpret_cast<const uint8_t*>(impl_->abi_sprites.data());
    }
    AYTHER_LEGACY_READ_BEGIN
    if (count) *count = impl_->runner.parsed_sprite_count();
    return impl_->runner.parsed_sprites();
    AYTHER_LEGACY_READ_END
}

void* AytherSession::core_export(const char* name) const {
    return impl_->runner.core_sym<void*>(name);
}

// ---------------------------------------------------------------------------
// E-7 (): las capas nativas del VDP en UNA llamada
// ---------------------------------------------------------------------------
namespace {
/// El export directo del fork. No vive en `ayther_interface_v1`: se resuelve
/// por símbolo, como las sondas de los spikes.
using AytherMultilayerFn = int32_t (AYTHER_CALL *)(
    uint16_t*, uint16_t*, uint16_t*, uint16_t*, uint16_t*,
    uint32_t, uint32_t, uint32_t*, uint32_t*);

/// El motivo, en castellano y en términos del JUEGO — tres de los cuatro no son
/// errores nuestros sino lo que la ROM está haciendo en ese frame.
const char* multilayer_motivo(int32_t st) {
    switch (st) {
        case AYTHER_STATUS_RC_NOT_MODE5:
            return "el juego no esta en modo grafico 5 (la recomposicion solo "
                   "existe para ese modo)";
        case AYTHER_STATUS_RC_INTERLACE2:
            return "interlace doble activo";
        case AYTHER_STATUS_RC_NTSC_FILTER:
            return "el filtro NTSC del core esta activo";
        case AYTHER_STATUS_RC_INVALID_PARAMS:
            return "parametros invalidos (buffer chico o viewport vacio)";
        // ABI 1.9: el journal raster paso de 256 eventos. El core NO
        // reproduce un prefijo del frame —una imagen plausible y equivocada
        // que nadie detectaria— y lo dice; la salida es el frame emitido.
        case AYTHER_STATUS_RC_JOURNAL_OVERFLOW:
            return "el journal raster desbordo (>256 eventos): fallback al "
                   "frame emitido, no un prefijo";
        case AYTHER_STATUS_UNSUPPORTED_MODE:
            return "un control pedido no tiene referente en el modo grafico "
                   "actual (p. ej. plano B en Mode 4)";
        case AYTHER_STATUS_DELTA_HISTORY_LOST:
            return "la generacion pedida salio del ring del delta: todo sucio";
        // Este NO viene del core: lo pone la sesion cuando le piden capas de un
        // frame que todavia no existe. Merece texto propio — «parametros
        // invalidos» mandaria a buscar el defecto adentro del core.
        case AYTHER_STATUS_INVALID_ARGUMENT:
            return "todavia no hay frame producido (llamar despues de step)";
        case AYTHER_STATUS_NOT_SUBSCRIBED:
            return "la sesion no esta suscripta a RECOMPOSITION";
        case AYTHER_STATUS_BUSY:
            return "se pidio con un frame a medio correr";
        case AYTHER_STATUS_UNSUPPORTED:
            return "el core no soporta la recomposicion multicapa";
        default:
            return "motivo desconocido";
    }
}
}  // namespace

AytherSession::Layers AytherSession::recompose_layers() {
    Impl& im = *impl_;
    Layers out;

    auto fallar = [&](int32_t st) {
        im.layers_error_status = st;
        // UNA vez por motivo, no una por frame: a 60 Hz un log por frame deja
        // de ser un diagnostico y pasa a tapar todo lo demas.
        if (im.layers_error_logged != st) {
            im.layers_error_logged = st;
            std::fprintf(stderr, "[AytherSession] capas VDP no disponibles: %s "
                                 "(status %d)\n", multilayer_motivo(st), st);
        }
        return out;   // vacio: ni composite ni capas (transaccional)
    };

    if (!im.runner.has_ayther_v1()) return fallar(AYTHER_STATUS_UNSUPPORTED);
    const ayther_interface_v1* api = im.runner.ayther_api();
    if (!(api->capabilities & AYTHER_CAP_RECOMPOSE_V1))
        return fallar(AYTHER_STATUS_UNSUPPORTED);
    if (!(im.ayther_subs_requested & AYTHER_SUB_RECOMPOSITION))
        return fallar(AYTHER_STATUS_NOT_SUBSCRIBED);

    // El símbolo se resuelve UNA vez: es un lookup en la tabla de exports del
    // DLL y esto se llama por frame.
    //
    // DOS CAMINOS, y el orden importa. Desde la ABI 1.2 la multicapa vive en el
    // DESCRIPTOR y el símbolo suelto `ayther_recompose_multilayer` YA NO SE
    // EXPORTA en el perfil standard: contra un core >= 1.2 el GetProcAddress
    // devuelve NULL y la feature queda muda sin un error a la vista. Se prueba
    // primero el descriptor —que es donde vive hoy— y se cae al export suelto
    // para los cores 1.0/1.1, que siguen siendo válidos porque la ABI es
    // aditiva y nada obliga a actualizar el binario.
    if (!im.multilayer_fn_resolved) {
        im.multilayer_fn_resolved = true;
        if (AYTHER_IFACE_HAS(api, recompose_multilayer) && api->recompose_multilayer)
            im.multilayer_fn = reinterpret_cast<void*>(api->recompose_multilayer);
        else
            im.multilayer_fn = reinterpret_cast<void*>(
                im.runner.core_sym<AytherMultilayerFn>("ayther_recompose_multilayer"));
    }
    if (!im.multilayer_fn) return fallar(AYTHER_STATUS_UNSUPPORTED);

    // Buffers de la sesión, dimensionados por el frame vivo. El core escribe
    // width*height compacto en cada uno.
    const uint32_t w = im.view.fb_width, h = im.view.fb_height;
    if (!w || !h) return fallar(AYTHER_STATUS_INVALID_ARGUMENT);
    const size_t px = size_t(w) * h;
    for (auto& buf : im.layer_bufs)
        if (buf.size() < px) buf.resize(px);

    uint32_t rw = 0, rh = 0;
    const auto fn = reinterpret_cast<AytherMultilayerFn>(im.multilayer_fn);
    const int32_t st = fn(im.layer_bufs[1].data(),   // plano A
                          im.layer_bufs[0].data(),   // plano B
                          im.layer_bufs[2].data(),   // ventana
                          im.layer_bufs[3].data(),   // sprites
                          im.layer_bufs[4].data(),   // composite
                          uint32_t(px), /*flags=*/0, &rw, &rh);
    if (st != AYTHER_STATUS_OK) return fallar(st);
    // Que devuelva OK con otras dimensiones que las del frame vivo significaria
    // que se recompuso OTRO frame: publicarlo seria peor que no publicar nada.
    if (rw != w || rh != h) return fallar(AYTHER_STATUS_RC_INVALID_PARAMS);

    im.layers_error_status = AYTHER_STATUS_OK;
    im.layers_error_logged = AYTHER_STATUS_OK;
    out.bg_b      = im.layer_bufs[0].data();
    out.bg_a      = im.layer_bufs[1].data();
    out.window    = im.layer_bufs[2].data();
    out.sprites   = im.layer_bufs[3].data();
    out.composite = im.layer_bufs[4].data();
    out.width     = w;
    out.height    = h;
    return out;
}

const char* AytherSession::layers_error() const noexcept {
    const int32_t st = impl_->layers_error_status;
    return st == AYTHER_STATUS_OK ? "" : multilayer_motivo(st);
}

void AytherSession::scene_judge_stats(uint32_t* occs, uint32_t* dropped,
                                      uint32_t* opaque, uint32_t* hits) const {
    const Impl& im = *impl_;
    if (occs)    *occs    = im.judge_occs;
    if (dropped) *dropped = im.judge_dropped;
    if (opaque)  *opaque  = im.judge_opaque;
    if (hits)    *hits    = im.judge_hits;
}

size_t AytherSession::scene_inventory(std::vector<SceneElement>& out) const {
    // R-3 (): la lista única de elementos dibujables del último frame
    // producido, en orden de dibujo global (back→front). No re-deriva nada:
    // junta lo que produce_frame ya computó (plane_cells del walk scroll-aware
    // + occs del hasher) y completa el patrón de los sprites desde la SAT
    // parseada del fork (attr = tile|flips|pal|pri).
    out.clear();
    const Impl& im = *impl_;
    const FrameView& v = im.view;

    // SAT parseada cruda (10 bytes/entrada: yr,xr,attr u16 LE + w,h,sat_idx,
    // chain_pos u8) — patrón + orden REAL de dibujo entre sprites (chain).
    struct Raw { uint16_t xr, yr, attr; uint8_t sat_idx, chain; };
    uint8_t rn = 0;
    const uint8_t* rp = parsed_sprites_raw(&rn);
    std::vector<Raw> raw;
    raw.reserve(rn);
    for (uint8_t i = 0; rp && i < rn; ++i) {
        const uint8_t* p = rp + (size_t)i * 10;
        Raw r;
        r.yr      = (uint16_t)(p[0] | (p[1] << 8));
        r.xr      = (uint16_t)(p[2] | (p[3] << 8));
        r.attr    = (uint16_t)(p[4] | (p[5] << 8));
        r.sat_idx = p[8];
        r.chain   = p[9];
        raw.push_back(r);
    }
    // occ → entrada de la SAT parseada: por slot; si el slot se reescribió a
    // mitad de frame (entradas duplicadas), desempata la posición.
    auto raw_of = [&](const AytherSpriteOccurrence& oc) -> const Raw* {
        const Raw* best = nullptr;
        for (const Raw& r : raw) {
            if (r.sat_idx != oc.slot) continue;
            if (!best) best = &r;
            if ((int)r.xr - 128 == (int)oc.screen_x &&
                (int)r.yr - 128 == (int)oc.screen_y) return &r;
        }
        return best;
    };

    // Joins de subs HD ya resueltos: sprite_subs por slot SAT (el array
    // paralelo sprite_sub_slot); plane_tile_subs 1×1 por posición exacta.
    int32_t slot2sub[80];
    for (int i = 0; i < 80; ++i) slot2sub[i] = -1;
    if (v.sprite_sub_slot)
        for (uint32_t i = 0; i < v.sprite_sub_count; ++i)
            if (v.sprite_sub_slot[i] < 80 && slot2sub[v.sprite_sub_slot[i]] < 0)
                slot2sub[v.sprite_sub_slot[i]] = (int32_t)i;
    auto plane_sub_at = (v.plane_tile_sub_count && v.plane_tile_subs)
        ? [](const FrameView& fv, int16_t x, int16_t y) -> int32_t {
              for (uint32_t i = 0; i < fv.plane_tile_sub_count; ++i) {
                  const AytherSpriteSub& q = fv.plane_tile_subs[i];
                  if (q.screen_x == x && q.screen_y == y &&
                      q.w_tiles == 1 && q.h_tiles == 1) return (int32_t)i;
              }
              return -1;
          }
        : [](const FrameView&, int16_t, int16_t) -> int32_t { return -1; };

    // R-5 (): MÁSCARA de sprites (x=0) — semántica del VDP que la lista
    // parseada no aplana (parse_satb agrega el sprite aunque el render lo
    // enmascare). Por línea y en orden GLOBAL de cadena: un sprite con x cruda
    // 0, después de al menos uno con x≠0 en esa línea, enmascara a los que
    // siguen en la cadena. El enmascarado en TODAS sus líneas visibles no se
    // emite (lo que no gana no se emite — R-1: «se modela», Sonic 2 la usa).
    // El enmascarado PARCIAL se emite entero (divergencia rara y acotada; el
    // presupuesto de píxeles por línea NO se replica — decisión de R-1).
    std::vector<const AytherSpriteOccurrence*> chain_order;
    for (uint32_t i = 0; i < v.sprite_occ_count; ++i)
        chain_order.push_back(&v.sprite_occs[i]);
    std::stable_sort(chain_order.begin(), chain_order.end(),
                     [&](const AytherSpriteOccurrence* a,
                         const AytherSpriteOccurrence* b) {
                         const Raw* ra = raw_of(*a); const Raw* rb = raw_of(*b);
                         return (ra ? ra->chain : 255) < (rb ? rb->chain : 255);
                     });
    std::unordered_set<const AytherSpriteOccurrence*> fully_masked;
    if (!chain_order.empty()) {
        // GPX: el sprite en x=0 no se enmascara a sí mismo — enmascara a los
        // SIGUIENTES de la cadena en esa línea (y requiere al menos un sprite
        // previo con x≠0). Un sprite queda fuera si TODAS sus líneas visibles
        // cayeron detrás de la máscara.
        const int fh = (int)v.fb_height;
        std::vector<int> vis(chain_order.size(), 0), msk(chain_order.size(), 0);
        for (int line = 0; line < fh; ++line) {
            bool masking = false, seen_nonzero = false;
            for (size_t k = 0; k < chain_order.size(); ++k) {
                const AytherSpriteOccurrence* oc = chain_order[k];
                if (line < oc->screen_y || line >= oc->screen_y + oc->h_tiles * 8)
                    continue;
                ++vis[k];
                if (masking) ++msk[k];
                const Raw* r = raw_of(*oc);
                if (r && r->xr != 0) seen_nonzero = true;
                else if (r && r->xr == 0 && seen_nonzero) masking = true;
            }
        }
        for (size_t k = 0; k < chain_order.size(); ++k)
            if (vis[k] > 0 && msk[k] == vis[k])
                fully_masked.insert(chain_order[k]);
    }

    // ---- : el FRAMEBUFFER como juez de qué sprite se dibujó ------------
    // La lista de sprites del fork es ACUMULATIVA dentro del frame — por diseño,
    // para que aparezca lo que el VDP procesó aunque el juego reescriba el SAT a
    // mitad de frame (el genio del logo de Aladdin). El efecto colateral aparece
    // cuando el juego deja basura y NO la vuelve a dibujar: la pantalla del mapa
    // de Golden Axe (f15328) publica 60 sprites y el framebuffer del core sólo
    // muestra unos pocos. Esos fantasmas entraban a la escena y se veían como
    // parches sobre el pergamino, con recortes negros donde el compose suprimía
    // el fondo para un HD que no existe.
    //
    // El framebuffer ES la verdad: un sprite que no aportó NI UN píxel a la
    // imagen no se dibujó. Se muestrean unos píxeles OPACOS de su patrón y se
    // comparan con el frame; sin una sola coincidencia, el sprite no entra a la
    // escena. Las occurrences NO se tocan: la autoría, el matching y el
    // inventario de poses siguen viendo exactamente lo mismo que antes.
    std::unordered_set<const AytherSpriteOccurrence*> not_drawn;
    {
        const uint8_t* jvram = im.vram_ptr();
        const uint8_t* jcram = im.cram_ptr();
        const size_t   jvsz  = im.runner.video_ram_size();
        const size_t   jcsz  = im.runner.color_ram_size();
        const uint8_t* fb    = static_cast<const uint8_t*>(v.fb_pixels);
        int jueces = 0, jopacas = 0, jaciertos = 0;
        if (fb && v.fb_width && v.fb_height && v.fb_pitch && jvram && jcram &&
            jvsz && jcsz >= 128) {
            auto vrd = [&](uint32_t o) -> uint8_t {
                o ^= 1u; return o < jvsz ? jvram[o] : 0;
            };
            //  EM-9.4: la tercera copia, ahora del header con oraculo.
            auto c8 = [](int c) { return ayther::cram_c8((uint8_t)c); };
            // El píxel del frame en RGB, sea cual sea el formato del core.
            auto fb_rgb = [&](int x, int y, int& r, int& g, int& b) -> bool {
                if (x < 0 || y < 0 || x >= (int)v.fb_width || y >= (int)v.fb_height)
                    return false;
                const uint8_t* row = fb + (size_t)y * v.fb_pitch;
                if (v.fb_format == 1) {            // XRGB8888
                    const uint8_t* p8 = row + (size_t)x * 4;
                    b = p8[0]; g = p8[1]; r = p8[2];
                } else {                            // RGB565 (0RGB1555 idem aprox)
                    const uint16_t p16 = (uint16_t)(row[(size_t)x * 2] |
                                                    (row[(size_t)x * 2 + 1] << 8));
                    if (v.fb_format == 2) {         // RGB565
                        r = ((p16 >> 11) & 0x1F) << 3;
                        g = ((p16 >>  5) & 0x3F) << 2;
                        b = ( p16        & 0x1F) << 3;
                    } else {                        // 0RGB1555
                        r = ((p16 >> 10) & 0x1F) << 3;
                        g = ((p16 >>  5) & 0x1F) << 3;
                        b = ( p16        & 0x1F) << 3;
                    }
                }
                return true;
            };
            // El patrón sale de la ENTRADA DEL SAT del slot, no de la SAT
            // parseada: `scene_inventory` no siempre tiene esa copia (llega
            // vacía y con ella el pattern del elemento queda en 0), y el juez
            // se quedaba sin nada que comparar.
            const uint8_t* jregs = im.regs_ptr();
            const size_t   jrsz  = im.runner.vdp_regs_size();
            const bool jregs_ok = jregs && jrsz >= 6;
            const uint32_t sat_base =
                jregs_ok ? ((uint32_t)(jregs[5] & 0x7F) << 9) : 0u;
            for (uint32_t i = 0; jregs_ok && i < v.sprite_occ_count; ++i) {
                const AytherSpriteOccurrence& oc = v.sprite_occs[i];
                if (oc.slot >= 80) continue;
                const uint32_t se = sat_base + (uint32_t)oc.slot * 8u;
                const uint16_t attr =
                    (uint16_t)((vrd(se + 4) << 8) | vrd(se + 5));
                const uint16_t base = (uint16_t)(attr & 0x7FF);
                if (!base) continue;                     // patrón 0 = vacío
                const int  wt = oc.w_tiles, ht = oc.h_tiles;
                const bool hf = oc.hflip != 0, vf = oc.vflip != 0;
                int opacas = 0, aciertos = 0;
                // Muestreo disperso: alcanza con unas pocas coincidencias, y un
                // sprite entero de 4x4 tiles son 1024 píxeles que no hace falta
                // recorrer por frame.
                for (int sc = 0; sc < wt && opacas < 24; ++sc)
                    for (int sr = 0; sr < ht && opacas < 24; ++sr) {
                        const int pc = hf ? (wt - 1 - sc) : sc;
                        const int pr = vf ? (ht - 1 - sr) : sr;
                        const uint16_t pat =
                            (uint16_t)(base + pc * ht + pr) & 0x7FF;
                        for (int row = 1; row < 8 && opacas < 24; row += 3)
                            for (int col = 1; col < 8 && opacas < 24; col += 3) {
                                const int tx = hf ? 7 - col : col;
                                const int ty = vf ? 7 - row : row;
                                const uint8_t byte =
                                    vrd(pat * 32u + ty * 4 + (tx >> 1));
                                const int idx = (tx & 1) ? (byte & 0xF) : (byte >> 4);
                                if (idx == 0) continue;   // transparente
                                ++opacas;
                                const size_t ce = (size_t)(oc.palette * 16 + idx) * 2;
                                if (ce + 1 >= jcsz) continue;
                                const uint16_t cv =
                                    (uint16_t)(jcram[ce] | (jcram[ce + 1] << 8));
                                const int er = c8(cv & 7), eg = c8((cv >> 3) & 7),
                                          eb = c8((cv >> 6) & 7);
                                int fr = 0, fg = 0, fb2 = 0;
                                if (!fb_rgb(oc.screen_x + sc * 8 + col,
                                            oc.screen_y + sr * 8 + row, fr, fg, fb2))
                                    continue;
                                // Tolerancia: el core expande 3→8 bits con su
                                // propia tabla y el formato del frame puede ser
                                // de 16 bits.
                                if (std::abs(fr - er) <= 24 && std::abs(fg - eg) <= 24 &&
                                    std::abs(fb2 - eb) <= 24)
                                    ++aciertos;
                            }
                    }
                // Sólo se descarta con evidencia: varias muestras opacas y NI UNA
                // en pantalla. Con pocas muestras (sprite casi transparente) no
                // se juzga — el falso descarte se vería peor que el fantasma.
                if (opacas >= 4 && aciertos == 0) not_drawn.insert(&oc);
                jueces++; jopacas += opacas; jaciertos += aciertos;
            }
            im.judge_occs = (uint32_t)jueces;
            im.judge_opaque = (uint32_t)jopacas;
            im.judge_hits = (uint32_t)jaciertos;
            im.judge_dropped = (uint32_t)not_drawn.size();
        }
    }


    // Capas de plano en orden de fondo→frente: B (plane 1), A (0), Window (2).
    auto emit_planes = [&](uint8_t pri) {
        static constexpr uint8_t kOrder[3] = { 1, 0, 2 };
        for (uint8_t pl : kOrder)
            for (uint32_t i = 0; i < v.plane_cell_count; ++i) {
                const PlaneCellHit& pc = v.plane_cells[i];
                if (pc.plane != pl || ((pc.flags >> 2) & 1) != pri) continue;
                SceneElement e;
                e.hash     = pc.hash;
                e.x        = pc.screen_x;
                e.y        = pc.screen_y;
                e.w = e.h  = 8;
                e.pattern  = pc.pattern;
                e.palette  = pc.palette;
                e.flips    = (uint8_t)(pc.flags & 3);
                e.layer    = pl == 1 ? 0 : pl == 0 ? 1 : 2;   // 0=B · 1=A · 2=W
                e.priority = pri;
                e.sub      = plane_sub_at(v, pc.screen_x, pc.screen_y);
                // Celda CONSUMIDA por un SET (Objeto): enlazar al quad del set
                // de SU MISMO plano que la contiene — con esto el compose
                // dibuja el HD del set en el z de la cadena (un sprite pri-1
                // queda DELANTE: las letras del título de GA sobre el
                // isologotipo) y la lane global lo saltea. El plano importa:
                // una celda del cielo (B) dentro del rect del kanji (A) NO es
                // del set — enlazarla reclamaba el cielo y dibujaba el logo
                // en el pase equivocado (bug del primer intento).
                const bool consumida = i < im.plane_cell_claimed.size() &&
                                       im.plane_cell_claimed[i];
                if (e.sub < 0 && consumida)
                    for (uint32_t q = 0; q < v.plane_tile_sub_count; ++q) {
                        const AytherSpriteSub& sq = v.plane_tile_subs[q];
                        if ((sq.w_tiles > 1 || sq.h_tiles > 1) &&
                            im.plane_tile_sub_plane[q] == pc.plane &&
                            pc.screen_x >= sq.screen_x &&
                            pc.screen_x < sq.screen_x + sq.w_tiles * 8 &&
                            pc.screen_y >= sq.screen_y &&
                            pc.screen_y < sq.screen_y + sq.h_tiles * 8) {
                            e.sub = (int32_t)q;
                            break;
                        }
                    }
                e.sub_kind = e.sub >= 0 ? 2 : 0;
                e.hidden   = im.plane_tiles_hidden.count(pc.hash) ? 1 : 0;
                // R-5: el ojo por CELDA de Editar (ex canal 0x104) también es
                // una propiedad del elemento — la máscara es 64×64 celdas de
                // pantalla, bit ty*64+tx.
                if (!e.hidden && im.tile_suppress_any &&
                    pc.screen_x >= 0 && pc.screen_y >= 0) {
                    const int tc = (pc.screen_y >> 3) * 64 + (pc.screen_x >> 3);
                    if ((pc.screen_x >> 3) < 64 && (pc.screen_y >> 3) < 64 &&
                        (im.tile_suppress_want[tc >> 3] & (1u << (tc & 7))))
                        e.hidden = 1;
                }
                // R-5: consumida por un matcher (Cuadro/Panorámica/set) o con
                // sub directo → el compose no dibuja el original con HD ON.
                e.claimed  = (e.sub >= 0 ||
                              (i < im.plane_cell_claimed.size() &&
                               im.plane_cell_claimed[i])) ? 1 : 0;
                // R-6: efectos asignados a este elemento (capa, hash).
                if (auto fit = im.element_fx[e.layer].find(e.hash);
                    fit != im.element_fx[e.layer].end()) {
                    const ElementEffect& fx = fit->second;
                    e.fx_tint[0] = fx.tint[0]; e.fx_tint[1] = fx.tint[1];
                    e.fx_tint[2] = fx.tint[2];
                    e.fx_opacity = fx.opacity;
                    e.fx_outline = fx.outline;
                }
                out.push_back(e);
            }
    };
    // Sprites del grupo de prioridad, del fondo al frente (cadena invertida:
    // menor chain = más al frente en el VDP → va último).
    auto emit_sprites = [&](uint8_t pri) {
        std::vector<const AytherSpriteOccurrence*> grp;
        for (uint32_t i = 0; i < v.sprite_occ_count; ++i)
            if (v.sprite_occs[i].priority == pri &&
                !fully_masked.count(&v.sprite_occs[i]))
                grp.push_back(&v.sprite_occs[i]);
        std::stable_sort(grp.begin(), grp.end(),
                         [&](const AytherSpriteOccurrence* a,
                             const AytherSpriteOccurrence* b) {
                             const Raw* ra = raw_of(*a); const Raw* rb = raw_of(*b);
                             return (ra ? ra->chain : 255) > (rb ? rb->chain : 255);
                         });
        for (const AytherSpriteOccurrence* oc : grp) {
            const Raw* r = raw_of(*oc);
            SceneElement e;
            e.hash     = oc->hash;
            e.x        = oc->screen_x;
            e.y        = oc->screen_y;
            e.w        = (uint8_t)(oc->w_tiles * 8);
            e.h        = (uint8_t)(oc->h_tiles * 8);
            e.pattern  = r ? (uint16_t)(r->attr & 0x7FF) : 0;
            e.palette  = oc->palette;
            e.flips    = (uint8_t)((oc->hflip ? 1 : 0) | (oc->vflip ? 2 : 0));
            e.layer    = 3;
            e.priority = pri;
            e.slot     = oc->slot;
            e.chain    = r ? r->chain : 0xFF;
            e.sub      = oc->slot < 80 ? slot2sub[oc->slot] : -1;
            e.sub_kind = e.sub >= 0 ? 1 : 0;
            e.hidden   = im.hidden_sprite_hashes.count(oc->hash) ? 1 : 0;
            // R-5: el ojo por SLOT de Editar (ex canal 0x103) idem.
            if (!e.hidden && im.suppress_any && oc->slot < 80 &&
                (im.suppress_want[oc->slot >> 3] & (1u << (oc->slot & 7))))
                e.hidden = 1;
            // R-5: TODOS los occs reclamados por un sub (miembros de la pose,
            // no sólo el ancla) — sin esto el original asoma bajo el HD.
            const size_t oidx = static_cast<size_t>(oc - v.sprite_occs);
            e.claimed  = (e.sub >= 0 ||
                          (oidx < kMaxSpriteOccs && im.sprite_claimed[oidx])) ? 1 : 0;
            // R-6: efectos asignados a este elemento (capa 3, hash).
            if (auto fit = im.element_fx[3].find(e.hash);
                fit != im.element_fx[3].end()) {
                const ElementEffect& fx = fit->second;
                e.fx_tint[0] = fx.tint[0]; e.fx_tint[1] = fx.tint[1];
                e.fx_tint[2] = fx.tint[2];
                e.fx_opacity = fx.opacity;
                e.fx_outline = fx.outline;
            }
            out.push_back(e);
        }
    };

    emit_planes(0);
    emit_sprites(0);
    emit_planes(1);
    emit_sprites(1);

    // 2026-07-31: OCULTAR UN MIEMBRO DESACTIVA EL ASSET DEL GRUPO.
    // Un asset HD reemplaza a un GRUPO de occurrences (la pose / el objeto), no
    // a una sola: el ancla dibuja el HD y los demás miembros quedan claimed
    // (originales suprimidos, el HD los tapa). Con eso, el ojo del Inspector no
    // se notaba — ocultar un miembro no cambiaba nada (el HD seguía tapándolo) y
    // ocultar el ancla tampoco. El contrato (mismo que el panel Sprites de
    // Posar): si CUALQUIER elemento del grupo está oculto, el asset se desactiva
    // y los miembros que siguen visibles vuelven a dibujar su original —
    // «abrir» la pose para ver lo que hay debajo es justamente para qué está el
    // ojo. Desactivar = claimed 0; el ancla conserva su `sub` para que el
    // renderer siga sabiendo que ese sub tiene ancla y la lane 4S no lo
    // re-dibuje plano encima.
    // Membresía: el sub cuyo rect contiene el CENTRO del elemento y es el MÁS
    // CHICO — gana el bbox más específico, igual que el picking de poses. Dos
    // poses solapadas (Tyris caminando bajo el dragón en GA) no se contagian:
    // cada occ cae en su propio sub, no en el grande que la contiene.
    if (v.sprite_sub_count && !out.empty()) {
        std::vector<int32_t> owner(out.size(), -1);
        std::vector<uint8_t> asset_off(v.sprite_sub_count, 0);
        for (size_t i = 0; i < out.size(); ++i) {
            const SceneElement& e = out[i];
            if (e.layer != 3 || !e.claimed) continue;
            int32_t own = e.sub;            // el ancla ya conoce su sub
            if (own < 0) {
                long best_area = 0;
                const int cx = e.x + e.w / 2, cy = e.y + e.h / 2;
                for (uint32_t s = 0; s < v.sprite_sub_count; ++s) {
                    const AytherSpriteSub& sb = v.sprite_subs[s];
                    const int rw = sb.w_px ? sb.w_px : sb.w_tiles * 8;
                    const int rh = sb.h_px ? sb.h_px : sb.h_tiles * 8;
                    if (cx < sb.screen_x || cx >= sb.screen_x + rw ||
                        cy < sb.screen_y || cy >= sb.screen_y + rh) continue;
                    const long area = (long)rw * rh;
                    if (own < 0 || area < best_area) {
                        own = (int32_t)s;
                        best_area = area;
                    }
                }
            }
            if (own < 0 || (uint32_t)own >= v.sprite_sub_count) continue;
            owner[i] = own;
            if (e.hidden) asset_off[own] = 1;
        }
        for (size_t i = 0; i < out.size(); ++i)
            if (owner[i] >= 0 && asset_off[owner[i]]) out[i].claimed = 0;
    }
    // : la mejora por software se resuelve DESPUÉS de la pasada anterior
    // (que puede des-reclamar elementos): sólo se mejora lo que NO reclamó un
    // HD — el asset ganó. Identidad (capa, hash), como hidden/fx.
    if (im.element_enhance_any) {
        // : la mejora sigue a la FAMILIA de contenido de su capa. No es un
        // subsistema propio ( lista lo que el pack REEMPLAZA, y la mejora
        // no reemplaza nada: procesa el original), pero tampoco puede quedar
        // fuera del routing — con el perfil «original» del pack, que apaga
        // todos los subsistemas, era lo único HD que seguía dibujándose.
        //
        // El criterio es el del jugador, no el del formato: quien apaga los
        // sprites quiere sus sprites como eran, y un sprite EPX-eado no lo es.
        // OR dentro de la familia (y no un subsistema exacto por Identidad)
        // porque el catálogo llega expandido a (capa, hash): de qué tipo de
        // Identidad vino ya no se sabe acá, y apagar de más sorprende más que
        // apagar de menos.
        const bool spr_family =
            im.sub_on(Subsystem::Sprites) || im.sub_on(Subsystem::Metasprites);
        const bool plane_family =
            im.sub_on(Subsystem::Tiles) || im.sub_on(Subsystem::Planes) ||
            im.sub_on(Subsystem::Ui);
        for (SceneElement& e : out) {
            e.fx_enhance = 0;
            // Reporte 2026-08-22: la mejora por software es contenido HD —
            // con el HD apagado (toggle Assets en Original, export MP4 o
            // snapshot en versión Original: todos bajan set_hd_enabled) no
            // se marca. El viewport renderiza siempre con hd_on=true (el
            // modo Original vive en la SESIÓN: feed de poses apagado, sets
            // apagados) — sin este gate, el EPX era lo único HD que seguía
            // dibujándose en Original.
            if (!im.hd_enabled) continue;
            if (e.claimed || e.layer >= 4) continue;
            if (!(e.layer == 3 ? spr_family : plane_family)) continue;
            const auto it = im.element_enhance[e.layer].find(e.hash);
            if (it == im.element_enhance[e.layer].end()) continue;
            e.fx_enhance   = 1;
            e.fx_enhance_k = it->second;   // 
        }
    }
    return out.size();
}

void AytherSession::set_layer_mask(uint8_t mask) {
    // Sólo guardar: produce_frame la aplica al frame visible y restaura los
    // sprites para la re-simulación bare (determinismo). Ver Impl::layer_mask_want.
    impl_->layer_mask_want = mask;
}

void AytherSession::set_layer_dim(bool on) {
    // Sólo guardar: produce_frame lo aplica al frame visible y lo limpia para la
    // re-sim bare. Sólo afecta la salida RGB (remap), no el estado del VDP.
    impl_->layer_dim_want = on;
}


void AytherSession::set_tile_suppress(const uint8_t* bits, size_t n) {
    Impl& im = *impl_;
    bool any = false;
    for (size_t i = 0; i < 512; ++i) {
        im.tile_suppress_want[i] = (bits && i < n) ? bits[i] : 0;
        any |= im.tile_suppress_want[i] != 0;
    }
    im.tile_suppress_any = any;
}

void AytherSession::set_plane_tile_hidden(const uint64_t* hashes, size_t n) {
    impl_->lab_plane_hidden.assign(hashes, hashes + (hashes ? n : 0));
    impl_->rebuild_hidden_sets();
}

void AytherSession::decode_plane_tile(uint16_t pattern, uint8_t pal, bool hflip,
                                      bool vflip, uint8_t* out_bgra) const {
    if (!out_bgra) return;
    const Impl& im = *impl_;
    const uint8_t* vram = im.vram_ptr();                   // E-5
    const size_t   vsz  = im.runner.video_ram_size();
    const uint8_t* cram = im.cram_ptr();                    // E-5
    const size_t   csz  = im.runner.color_ram_size();
    if (!vram || vsz == 0) return;
    auto rd = [&](uint32_t off) -> uint8_t {     // vista de bus (word-swapped)
        const uint32_t i = off ^ 1u;
        return i < vsz ? vram[i] : 0;
    };
    //  EM-9.4: la expansion de 3 bits a 8 vive en `cram_palette.h`, con su
    // oraculo. Estaba copiada en tres lugares y de ella dependen el hash de
    // tile de plano, la firma de variante por paleta () y el tinte — tres
    // copias sin test son una que se va a arreglar en dos lugares.
    auto c8 = [](int c) { return ayther::cram_c8(static_cast<uint8_t>(c)); };
    for (int row = 0; row < 8; ++row)
        for (int col = 0; col < 8; ++col) {
            const int    sx  = hflip ? 7 - col : col;
            const int    sy  = vflip ? 7 - row : row;
            const uint32_t off = static_cast<uint32_t>(pattern) * 32u + sy * 4 + (sx >> 1);
            const uint8_t  byte = rd(off);
            const int idx = (sx & 1) ? (byte & 0xF) : (byte >> 4);
            uint8_t* d = out_bgra + (static_cast<size_t>(row) * 8 + col) * 4;
            if (idx == 0) { d[0] = 0x28; d[1] = 0x20; d[2] = 0x20; d[3] = 0xFF; continue; }
            const size_t e = static_cast<size_t>(pal * 16 + idx) * 2;
            const uint16_t v = (cram && e + 1 < csz)
                ? static_cast<uint16_t>(cram[e] | (cram[e + 1] << 8)) : 0;
            // color_ram() devuelve la CRAM EMPAQUETADA (R=bits0-2, G=3-5, B=6-8),
            // no el formato Genesis "con huecos" (R=1-3,G=5-7,B=9-11). Verificado
            // contra el juego: blanco=0x1FF, azul=0x1E3 → R3 G4 B7 (ver
            // write_pose_snapshot_vram en lab/src/app/project_io.cpp).
            d[0] = c8((v >> 6) & 7);   // B
            d[1] = c8((v >> 3) & 7);   // G
            d[2] = c8(v & 7);          // R
            d[3] = 0xFF;
        }
}

void AytherSession::decode_plane_tile_rgba(uint16_t pattern, uint8_t pal, bool hflip,
                                           bool vflip, uint8_t* out_rgba) const {
    if (!out_rgba) return;
    const Impl& im = *impl_;
    const uint8_t* vram = im.vram_ptr();                   // E-5
    const size_t   vsz  = im.runner.video_ram_size();
    const uint8_t* cram = im.cram_ptr();                    // E-5
    const size_t   csz  = im.runner.color_ram_size();
    if (!vram || vsz == 0) return;
    auto rd = [&](uint32_t off) -> uint8_t {     // vista de bus (word-swapped)
        const uint32_t i = off ^ 1u;
        return i < vsz ? vram[i] : 0;
    };
    //  EM-9.4: la expansion de 3 bits a 8 vive en `cram_palette.h`, con su
    // oraculo. Estaba copiada en tres lugares y de ella dependen el hash de
    // tile de plano, la firma de variante por paleta () y el tinte — tres
    // copias sin test son una que se va a arreglar en dos lugares.
    auto c8 = [](int c) { return ayther::cram_c8(static_cast<uint8_t>(c)); };
    for (int row = 0; row < 8; ++row)
        for (int col = 0; col < 8; ++col) {
            const int    sx  = hflip ? 7 - col : col;
            const int    sy  = vflip ? 7 - row : row;
            const uint32_t off = static_cast<uint32_t>(pattern) * 32u + sy * 4 + (sx >> 1);
            const uint8_t  byte = rd(off);
            const int idx = (sx & 1) ? (byte & 0xF) : (byte >> 4);
            uint8_t* d = out_rgba + (static_cast<size_t>(row) * 8 + col) * 4;
            if (idx == 0) { d[0] = d[1] = d[2] = d[3] = 0; continue; }   // transparente (VDP)
            const size_t e = static_cast<size_t>(pal * 16 + idx) * 2;
            const uint16_t v = (cram && e + 1 < csz)
                ? static_cast<uint16_t>(cram[e] | (cram[e + 1] << 8)) : 0;
            // CRAM EMPAQUETADA (R=bits0-2, G=3-5, B=6-8) — mismo criterio que
            // decode_plane_tile arriba; el formato Genesis "con huecos"
            // (R=1-3,G=5-7,B=9-11) acá corría los colores (MAGIC amarillo).
            d[0] = c8(v & 7);          // R
            d[1] = c8((v >> 3) & 7);   // G
            d[2] = c8((v >> 6) & 7);   // B
            d[3] = 0xFF;
        }
}

// -- Fondos (Componentes): captura del stitcher + export por capa --------------
void AytherSession::bg_capture(bool on) {
    Impl& im = *impl_;
    if (on) {
        im.bg_st.reset(ayther_bg_stitcher_new());   // captura fresca
        im.bg_uxA.reset();                          // unwrappers lazy (wpx/hpx del VDP)
        im.bg_uyA.reset();
        im.bg_uxB.reset();
        im.bg_uyB.reset();
        im.bg_scene_cut = false;                    // : re-armar el corte
        for (auto& m : im.bg_hash) m.clear();
        for (auto& m : im.bg_hash_drawn) m.clear();
        // : cámara de contenido fresca (el offset es POR captura).
        for (int p = 0; p < 3; ++p) { im.bg_content_col[p] = 0; im.bg_content_row[p] = 0; }
        // : las camaras por banda tambien. Arrastrar el absoluto viejo a
        // otro nivel mandaria al stitcher a escribir cientos de columnas al vacio.
        for (int p = 0; p < 2; ++p) im.bg_bands[p].reset();
        for (int p = 0; p < 2; ++p) { im.bg_prev_grid[p].clear(); im.bg_prev_ok[p] = false;
                                      im.bg_static_frames[p] = 0; }
    }
    im.bg_capture_on = on;
}

std::vector<AytherSession::PanoramaCell>
AytherSession::bg_cells(uint8_t plane) const {
    std::vector<PanoramaCell> out;
    if (plane > 2) return out;
    const auto& m = impl_->bg_hash[plane];
    out.reserve(m.size());
    for (const auto& [k, h] : m)
        out.push_back({ h, (int32_t)(uint32_t)(k >> 32), (int32_t)(uint32_t)(k & 0xFFFFFFFFull) });
    // Puede haber VARIAS entradas por posición (una por estado de una celda
    // animada). Es a propósito: cada estado tiene que poder anclar.
    //
    // : pero DENTRO de una posición el orden ya no es indiferente — la
    // celda que la lámina DIBUJA va primero. El consumidor
    // (`build_panorama`) toma la primera como la dibujada y verifica la
    // cobertura contra ella; las demás siguen sirviendo para anclar.
    //
    // Sin esto, la cobertura se comprobaba contra CUALQUIERA de los hashes que
    // pasaron por la posición, y una tira ambigua daba «100 %» con el PNG
    // mostrando otro tramo del nivel. El área nativa lo tapa —las celdas vivas
    // que la tira no reclamó se dibujan encima— y el área extendida no tiene
    // con qué corregirse: ahí se ve exactamente lo que la tira tiene.
    //
    // Orden ESTABLE: el mapa no lo tiene, y la definición de una Panorámica se
    // hornea a TOML — sin esto dos barridos idénticos darían archivos distintos.
    const auto& drawn = impl_->bg_hash_drawn[plane];
    std::sort(out.begin(), out.end(),
              [&](const PanoramaCell& a, const PanoramaCell& b) {
        if (a.ly != b.ly) return a.ly < b.ly;
        if (a.lx != b.lx) return a.lx < b.lx;
        // Misma posición: la dibujada primero. El resto, por hash, para que el
        // orden siga siendo total y el horneado, reproducible byte a byte.
        const auto it = drawn.find(Impl::pano_key(a.lx, a.ly));
        if (it != drawn.end()) {
            if (a.hash == it->second) return true;
            if (b.hash == it->second) return false;
        }
        return a.hash < b.hash;
    });
    return out;
}

std::vector<std::pair<std::string, std::string>>
AytherSession::core_options_declared() const {
    return impl_->runner.declared_options();
}

bool AytherSession::bg_capturing() const noexcept { return impl_->bg_capture_on; }

size_t AytherSession::bg_cell_count(uint8_t plane) const noexcept {
    const Impl& im = *impl_;
    return im.bg_st ? ayther_bg_stitcher_cell_count(im.bg_st.get(), plane) : 0;
}

Result<std::vector<std::string>> AytherSession::export_backgrounds(const std::string& out_dir) {
    Impl& im = *impl_;
    std::vector<std::string> written;
    if (!im.bg_st) return written;
    const TileDecodeFn decode = [this](uint16_t p, uint8_t pal, bool hf, bool vf, uint8_t* out) {
        decode_plane_tile_rgba(p, pal, hf, vf, out);
    };
    for (uint8_t plane = 0; plane < 3; ++plane) {
        const LayerImage img = BackgroundExporter::build(im.bg_st.get(), plane, decode);
        if (img.cell_count == 0) continue;
        const std::string path = out_dir + "/" + BackgroundExporter::filename(img);
        auto w = BackgroundExporter::write_png(img, path);
        if (!w) return w.error;
        written.push_back(path);
    }
    return written;
}

bool AytherSession::bg_bounds(uint8_t plane, int32_t out[4]) const {
    const Impl& im = *impl_;
    if (!im.bg_st || plane > 2 || !out) return false;
    return ayther_bg_stitcher_bounds(im.bg_st.get(), plane, out);
}

Result<void> AytherSession::export_background_plane(uint8_t plane,
                                                    const std::string& path) {
    Impl& im = *impl_;
    if (!im.bg_st)
        return Result<void>::fail(ErrorCode::BadFormat,
                                  "background export: sin captura de fondos");
    const TileDecodeFn decode = [this](uint16_t p, uint8_t pal, bool hf, bool vf, uint8_t* out) {
        decode_plane_tile_rgba(p, pal, hf, vf, out);
    };
    const LayerImage img = BackgroundExporter::build(im.bg_st.get(), plane, decode);
    if (img.cell_count == 0)
        return Result<void>::fail(ErrorCode::BadFormat,
                                  "background export: el plano no acumuló celdas");
    return BackgroundExporter::write_png(img, path);
}

void AytherSession::set_sprite_suppress(const uint8_t* bits, size_t n) {
    Impl& im = *impl_;
    bool any = false;
    for (size_t i = 0; i < 16; ++i) {
        im.suppress_want[i] = (bits && i < n) ? bits[i] : 0;
        any |= im.suppress_want[i] != 0;
    }
    im.suppress_any = any;
}

void AytherSession::cheat_set(unsigned index, bool enabled, const char* code) {
    impl_->runner.cheat_set(index, enabled, code);
}

void AytherSession::cheat_reset() { impl_->runner.cheat_reset(); }

// Vista 68k: el array del core esta word-swapped en hosts LE (addr^1) — el
// spike maper_probe lo verifico contra el timer de Sonic 2. Centralizado aca
// para que TODO el Lab (explorer, mapa, arbol, poke, MCP) hable el mismo
// espacio de direcciones que las bases documentadas.
uint8_t AytherSession::ram_u8(uint32_t off) const noexcept {
    const size_t   n = impl_->runner.work_ram_size();
    const uint8_t* r = impl_->runner.work_ram();
    const uint32_t i = off ^ 1u;
    return (r && i < n) ? r[i] : 0;
}

uint16_t AytherSession::ram_u16(uint32_t off) const noexcept {
    return static_cast<uint16_t>((uint16_t(ram_u8(off)) << 8) | ram_u8(off + 1));
}

uint32_t AytherSession::ram_u32(uint32_t off) const noexcept {
    return (uint32_t(ram_u16(off)) << 16) | ram_u16(off + 2);
}

// Poke (M5): escribe en la vista 68k y ENSUCIA la sesión — la escritura no
// vive en ningún input stream, así que un replay no la reproduce: REC queda
// bloqueado hasta unserialize (marcador) o reset, que limpian el flag.
bool AytherSession::poke(uint32_t off, const uint8_t* data, size_t len) {
    uint8_t*     r = impl_->runner.work_ram_mut();
    const size_t n = impl_->runner.work_ram_size();
    if (!r || !data || !len || off + len > n) return false;
    for (size_t i = 0; i < len; ++i)
        r[(off + i) ^ 1u] = data[i];   // host LE: array word-swapped
    impl_->poke_dirty = true;
    return true;
}

// -- RAM del Z80 () ------------------------------------------------------

const uint8_t* AytherSession::z80_ram() const noexcept {
    return impl_->runner.z80_ram();
}
size_t AytherSession::z80_ram_size() const noexcept {
    return impl_->runner.z80_ram_size();
}
bool AytherSession::z80_poke(uint32_t off, const uint8_t* data, size_t len) {
    uint8_t*     z = impl_->runner.z80_ram_mut();
    const size_t n = impl_->runner.z80_ram_size();
    if (!z || !data || !len || off + len > n) return false;
    // SIN word-swap, a diferencia de `poke`: la RAM del Z80 es de un
    // procesador de 8 bits y el fork la publica tal cual. Aplicar el `^1` acá
    // escribiría los bytes cruzados, que es el error que la vista 68k de work
    // RAM existe para evitar en el otro lado.
    std::memcpy(z + off, data, len);
    impl_->poke_dirty = true;   // escribir fuera del input stream ensucia la sesion
    return true;
}

// -- Cheats del jugador ( EM-7.3) ---------------------------------------

void AytherSession::add_cheat(uint32_t address, uint16_t value) {
    impl_->cheats.push_back({ address, value });
}
void AytherSession::clear_cheats() noexcept { impl_->cheats.clear(); }
uint32_t AytherSession::cheat_count() const noexcept {
    return (uint32_t)impl_->cheats.size();
}

bool AytherSession::dirty() const noexcept { return impl_->poke_dirty; }
void AytherSession::clear_dirty() noexcept { impl_->poke_dirty = false; }

// ---------------------------------------------------------------------------
// Read-only introspection
// ---------------------------------------------------------------------------
const char* AytherSession::game_id() const noexcept {
    return impl_->pack ? ayther_pack_game_id(impl_->pack.get()) : "";
}
double AytherSession::timing_fps() const noexcept { return impl_->runner.fps(); }
uint64_t AytherSession::audio_starved_frames() const noexcept {
    return impl_->audio.starved_frames();
}
float AytherSession::audio_drc_ratio() const noexcept {
    return impl_->audio.drc_ratio();
}
float AytherSession::audio_backlog_avg() const noexcept {
    return impl_->audio.drc_queue_avg();
}

AyArchive*     AytherSession::pack()          const noexcept { return impl_->pack.get(); }
const uint8_t* AytherSession::work_ram()      const noexcept { return impl_->runner.work_ram(); }
size_t         AytherSession::work_ram_size() const noexcept { return impl_->runner.work_ram_size(); }

void AytherSession::dump_tile_catalog(const char* path) const {
    ayther_tile_hasher_dump_toml(impl_->tile_hasher.get(), path);
}

}  // namespace ayther
