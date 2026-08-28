#pragma once
// ---------------------------------------------------------------------------
// audio_player.h — SDL3 audio device: emulator passthrough + HD WAV playback.
//
// v0.9.0:  PCM passthrough via continuous emu_stream_ + one-shot HD SFX streams.
// v0.9.1:  Mute-on-substitution — emulator PCM is suppressed for any hash that
//          had a resolved HD substitution in the previous tick.  This left a
//          1-tick (~16 ms) bleed on the first appearance of a new substitution,
//          because the audio callback fires *during* run_frame(), before the
//          substitution for the current frame has been resolved.
// v0.9.7:  Deferred passthrough — the audio callback no longer pushes PCM
//          directly.  It buffers each batch (tagged with its hash) via
//          buffer_emulator(); the main loop resolves substitutions for the
//          *same* frame, refreshes the mute set, then calls flush_emulator(),
//          which pushes only the non-muted batches.  The mute decision now uses
//          the current frame's resolution, eliminating the 1-tick bleed.
//
// Design:
//   emu_stream_     Continuous emulator PCM passthrough (S16 stereo, 44100 Hz).
//
//   pending_pcm_    Per-frame interleaved PCM staged by buffer_emulator() and
//   pending_batches_  drained by flush_emulator().  Capacity is reused across
//                   frames (no steady-state allocation).
//
//   sfx_streams_    One-shot streams for HD WAV substitutions.
//                   Created in play_substitutions(), reaped in tick() once drained.
//
//   mute_hashes_    Set of hashes whose emulator PCM should be suppressed this
//                   frame.  Refreshed via set_mute_hashes() from resolved subs.
//
//   wav_cache_      WAV assets decoded once per asset_path and held in memory.
//                   Freed in shutdown().
//
// Thread model: all public methods are called from the main/emulation thread.
// SDL3 audio streams are internally thread-safe for the push side.
//
// Typical per-tick call sequence:
//
//   // inside runner audio callback (called during run_frame()):
//   uint64_t hash = ayther_audio_hasher_process_batch(hasher, data, frames);
//   audio_player.buffer_emulator(hash, data, frames);
//
//   // after ayther_audio_sub_resolve() — unconditional, clears set when 0:
//   audio_player.play_substitutions(pack, audio_subs, n_audio_subs);
//   audio_player.set_mute_hashes(audio_subs, n_audio_subs);
//   audio_player.flush_emulator();   // pushes non-muted batches to emu_stream_
//
//   // end of frame:
//   audio_player.tick();
// ---------------------------------------------------------------------------

#include <SDL3/SDL.h>
#include "audio_hd_mixer.h"
#include "audio_asset_level.h"   // 
#include "ayther_core_ffi.h"

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

/// @brief Owns SDL audio resources and aligns native and replacement audio.
///
/// init() is fallible; shutdown() is idempotent and is also called by the
/// destructor. Unless a method states otherwise, pointers passed to this class
/// are borrowed for the duration of the call and their data is copied when it
/// must be retained.
///
/// This class is not thread-safe. Drive staging, substitution, and flush
/// operations from the session thread. SDL callbacks and stream ownership must
/// not outlive the player.
class AudioPlayer {
public:
    /// Explains why a replacement asset is unavailable. `None` means decoded
    /// and ready. Disk failures may be invalidated when the file appears or its
    /// fingerprint changes; failures for immutable pack content are permanent.
    enum class AssetError : uint8_t {
        None,         ///< Decoded PCM is available in the cache.
        Missing,      ///< The disk or pack entry cannot be opened.
        Empty,        ///< The asset exists but has no payload.
        Unsupported,  ///< No registered decoder accepts the extension.
        Corrupt,      ///< The decoder rejected the payload.
    };

    AudioPlayer()  = default;
    /// RAII: release the SDL device + streams. shutdown() is idempotent and safe
    /// on a never-initialised player, so owners (e.g. AytherSession) don't need a
    /// manual shutdown() call before destruction.
    ~AudioPlayer() { shutdown(); }

    // Holds raw SDL handles — non-copyable (a copy would double-free the device).
    AudioPlayer(const AudioPlayer&)            = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    // ---- Lifecycle ----------------------------------------------------------

    /// Open the default SDL3 audio device and create the emulator stream.
    /// Must be called after SDL_Init(SDL_INIT_AUDIO).
    /// Returns true on success; false leaves the player in a safe no-op state.
    bool init();

    /// Flush all one-shot streams, unbind the emulator stream, and close the
    /// audio device.  Safe to call even if init() was never called or failed.
    void shutdown();

    // ---- Mute-on-substitution (v0.9.1) --------------------------------------

    /// Returns true if emulator PCM with this hash should be suppressed because
    /// a HD substitution is active for it this frame.
    /// hash == 0 (silent batch) always returns false.
    bool should_mute(uint64_t hash) const;

    /// Refresh the mute set from the current frame's resolved substitutions.
    /// Pass count == 0 to clear (no active substitutions → passthrough restored).
    /// Call once per frame, after play_substitutions() and before flush_emulator().
    void set_mute_hashes(const AytherAudioSub* subs, uint32_t count);

    /// Persistent per-hash mute requested by the author (timeline AUDIO rows in
    /// Editar). Independent of HD substitutions — survives between frames and is
    /// OR'd into should_mute(). Pass n == 0 to clear (all sounds audible again).
    void set_user_mute_hashes(const uint64_t* hashes, size_t n);

    /// Sets gain for the complete game-audio stream; `1.0` is neutral. The
    /// stream includes routed synthesized audio, so this attenuates the game
    /// mix without enumerating individual identities.
    ///
    /// Intended for temporary ducking. The operation is idempotent and updates
    /// SDL only when the value changes.
    void set_game_gain(float g);
    float game_gain() const { return game_gain_; }

    // ---- Per-audio-callback -------------------------------------------------

    /// Stage one emulator PCM batch (S16LE stereo, 44100 Hz) tagged with its
    /// hash.  Called synchronously from the libretro audio_sample_batch
    /// trampoline inside run_frame() — i.e. on the main thread.
    /// The batch is not pushed to the device until flush_emulator() runs, by
    /// which time the current frame's substitutions have been resolved.
    void buffer_emulator(uint64_t hash, const int16_t* data, size_t frames);

    // ---- Per-frame ----------------------------------------------------------

    /// Push every staged batch whose hash is not muted to emu_stream_, then
    /// clear the staging buffers (capacity retained).  Call once per frame,
    /// after set_mute_hashes().
    ///
    /// In unified mode, muted native batches are replaced with silence rather
    /// than removed, preserving the block timeline. Replacement voices are
    /// added at their exact sample positions and the combined block crosses the
    /// same rate-control backlog. `suppress_original` silences the native block
    /// while preserving the replacement mix. It replaces discard-based range
    /// muting, which would also remove replacement audio from a unified block.
    void flush_emulator(bool suppress_original = false);

    /// Drop every staged emulator batch WITHOUT pushing it to the device, then
    /// clear the staging buffers (capacity retained).  Used by replay_seek's
    /// fast re-sim: the skipped frames' PCM piles up in pending_pcm_ via the
    /// audio callback and would otherwise blast out at once on the final
    /// produce_frame — discard it so a seek stays silent until its target frame.
    void discard_emulator();

    // ---- Per-tick -----------------------------------------------------------

    /// For each resolved AytherAudioSub, decode the WAV from the pack and
    /// start a one-shot playback stream.  Deduplicates by hash within the tick
    /// (and across ticks if the previous stream has not yet finished).
    void play_substitutions(AyArchive*            pack,
                            const AytherAudioSub* subs,
                            uint32_t              count);

    /// Reap one-shot streams that have finished playing (available bytes == 0).
    /// Call once per frame, after play_substitutions().
    void tick();

    /// Immediately stop and destroy all active one-shot streams.
    void stop_all_sfx();

    /// Corta YA los one-shot marcados `preview` (2026-08-22: cerrar la
    /// Biblioteca de Audios con un ▶ sonando). INMEDIATO y sin fade a
    /// propósito: el fade lo progresa tick(), que solo corre dentro del flush
    /// audible del produce — con el transporte en PAUSA (el caso típico con un
    /// diálogo abierto) un fade jamás termina y el preview drena entero.
    void stop_preview_sfx();

    /// : corte TOTAL del audio de GAMEPLAY al pausar el transporte.
    /// - detiene los SFX one-shot y event-streams del gameplay (los one-shot
    ///   marcados `preview` — previews de autoría — siguen sonando);
    /// - descarta el staging (pending_pcm_/pending_batches_);
    /// - vacía el PCM ya encolado en emu_stream_ y synth_stream_;
    /// - resetea el estado DRC (EMA, ratio, last_flush) para que el primer
    ///   flush tras reanudar entre por el camino de stall y re-cebe el colchón.
    /// Idempotente (repetir en pausa no hace nada nuevo ni filtra handles).
    /// Devuelve los CUADROS estéreo descartados (staging + emu + synth) y los
    /// acumula en pause_cut_frames() — telemetría de la pausa.
    uint64_t cut_transport_audio();

    /// Telemetría : cuadros descartados por cortes de pausa (acumulado) y
    /// cuántos cortes efectivos hubo (llamadas que descartaron algo).
    uint64_t pause_cut_frames() const { return pause_cut_frames_; }
    uint64_t pause_cuts()       const { return pause_cuts_; }

    // ---- HD por EVENTO (C-A2, Componentes) ----------------------------------

    /// Arranca el asset HD (WAV/OGG/FLAC del pack) de un EVENTO sustituido,
    /// alineado a su start_frame. Un retrigger de la misma `signature` REINICIA
    /// el stream (el evento volvió a detectarse). `looping`: el PCM se
    /// re-alimenta en tick_events() hasta que el frame pase `end_frame`; un
    /// asset no-loop suena una vez y su stream se recoge al drenar.
    /// : devuelve true solo si el stream quedó SONANDO en el device — el
    /// caller decide el mute del original con esta respuesta, no con la
    /// existencia de la asignación.
    /// : `cut_frame` = frame ABSOLUTO tras el cual tick_events destruye el
    /// stream aunque tenga PCM encolado (end_frame + tail de la política).
    /// UINT64_MAX = drena entero (contrato legacy de los non-loop). Un loop
    /// deja de re-alimentarse en end_frame y su resto drena hasta cut_frame.
    /// : `start_offset_seconds` arranca DESDE EL MEDIO del asset — la
    /// reanudación tras una pausa recrea el stream en el punto que dicta el
    /// reloj emulado. Para un loop el offset es módulo del asset (la fase se
    /// conserva); para un non-loop pasado el final devuelve true SIN crear
    /// stream (nada que sonar, no es un fallo — mismo contrato que el
    /// one-shot, ).
    /// : `fade_frames` > 0 = política de fin FADE_OUT — pasado end_frame
    /// la voz se desvanece en esos cuadros (44100/s) y muere en silencio, en
    /// vez de cortarse seco. Es ALTERNATIVA a tail, no acumulable: con fade,
    /// `cut_frame` no interrumpe la rampa. 0 = comportamiento de .
    bool play_event_hd(AyArchive* pack, const char* asset_path, bool looping,
                       uint64_t signature, uint64_t end_frame,
                       uint64_t cut_frame = UINT64_MAX,
                       double start_offset_seconds = 0.0,
                       uint32_t fade_frames = 0,
                       // : ganancia AUTORADA de la Secuencia. Va al final y
                       // con default neutro para que ningun llamador cambie: la
                       // ausencia del dato es 1.0, igual que en el TOML.
                       float gain = 1.0f,
                       // : region de loop en CUADROS del asset. (0,0) = el
                       // asset entero, que es lo que se hacia siempre. El mixer
                       // ya sabe ciclarla con modulo sobre el span; lo que
                       // faltaba era que el dato llegara hasta aca.
                       size_t loop_begin = 0, size_t loop_end = 0);

    /// Mantenimiento por frame de los event-streams: corta los loops que
    /// pasaron su end_frame, re-alimenta los activos que van quedando sin
    /// datos, y recoge los no-loop ya drenados. Llamar con el frame actual.
    void tick_events(uint64_t frame);

    /// Corta TODOS los event-streams ya (stop / scrub / cambio de toma).
    void stop_all_events();

    /// Reproduce un buffer PCM (S16 stereo, 44100 Hz) como one-shot — la vista
    /// previa de un audio del juego capturado por hash (panel Capas). Reemplaza
    /// el preview en curso. `frames` = cuadros estéreo (samples = frames×2).
    /// Encola PCM del sintetizador SoundFont (): estéreo INTERCALADO f32 a
    /// 44100, que es lo que entrega RustySynth y la misma tasa del emulador —
    /// así «un frame de juego = N muestras» es la misma cuenta para los dos.
    ///
    /// Es un stream CONTINUO, no un one-shot: se llama una vez por frame con
    /// exactamente las muestras de ese frame. Un `play_oneshot_pcm` por frame
    /// abriría un stream nuevo cada vez y se solaparían.
    void feed_synth(const float* interleaved, size_t frames);

    /// Cuántas muestras de PCM tiene STAGEADAS el emulador para este frame.
    ///
    /// Es la medida correcta de «cuánto audio vale este frame», y por eso la
    /// usa el sintetizador en vez de 44100/fps: si el Lab corre más lento que
    /// el tiempo real —y lo hace— un número fijo alimenta de menos y el stream
    /// del sintetizador se muere de hambre, mientras el del emulador se salva
    /// porque tiene DRC. Atándolos al mismo número, los dos derivan igual y no
    /// se separan.
    size_t pending_frames() const { return pending_pcm_.size() / 2; }

    /// Muestras que le quedan al stream del SINTETIZADOR sin consumir.
    ///
    /// Importa desde que el router () lo usa para TODO el audio. Con el
    /// SoundFont daba igual: entregaba notas sueltas y un hueco era silencio.
    /// El router entrega audio CONTINUO, y el Lab lo hace a tirones de 90-150 ms
    /// —no a 16,7— así que si el stream no lleva colchón, SDL lo vacía entre
    /// tirón y tirón y cada hueco es un corte audible.
    size_t synth_queued_frames() const;

    /// Mete `frames` muestras de silencio en el stream del sintetizador para
    /// armar ese colchón antes de empezar a entregar.
    void prime_synth(size_t frames);

    /// SUMA audio en el PCM del emulador todavía sin flushear.
    ///
    /// Es el camino del router (), y la diferencia con feed_synth no es
    /// cosmética: el stream del emulador tiene DRC —estira su ritmo cuando el
    /// Lab no llega a tiempo, que es lo que costó diez causas raíz en — y
    /// el del sintetizador no tiene nada. Con stream propio, el router entrega
    /// menos audio del que el device consume y SDL lo vacía: medido,
    /// cola_stream=0 en casi todos los ticks, o sea un corte por tirón.
    ///
    /// Montado en el PCM del emulador hereda todo el pacing ya resuelto.
    ///
    /// `mix_over_chip` decide qué pasa con lo que el chip dejó staged:
    ///
    ///   false — el bloque OCUPA SU LUGAR (lo staged se descarta). Es el caso
    ///           del cartucho: el router espeja los diez canales que suenan, así
    ///           que el PCM del core no aporta nada que no venga en el bloque, y
    ///           el chip puede seguir sonando entero para el hasher.
    ///   true  — el bloque se SUMA sobre el frame actual, conservando lo staged.
    ///           Es el caso del Sega CD (): ahí el buffer del core lleva el
    ///           chip PCM y el CDDA, que el router no espeja. Lo que el router
    ///           sí rinde ya vino callado del core por máscara, así que nada se
    ///           oye dos veces.
    void buffer_router(const float* interleaved, size_t frames,
                       bool mix_over_chip = false);

    /// Hash del lote del router. Reservado: el set de mute se arma con hashes
    /// de audio del juego, así que éste nunca cae ahí.
    static constexpr uint64_t kRouterHash = 0xA17E'2600'0000'0001ull;

    /// Descarta lo encolado del sintetizador. Para los cortes: un seek deja
    /// notas en vuelo que sonarían sobre la escena nueva.
    void clear_synth();

    void play_oneshot_pcm(const int16_t* pcm, size_t frames);
    /// Detiene la vista previa one-shot (botón Detener / al cerrar el diálogo).
    void stop_oneshot();

    /// Reproduce un ASSET HD SUELTO (de disco, no del pack) como one-shot SFX —
    /// la sustitución de audio por evento en autoría (workspace Audios, C-A4):
    /// decodifica WAV/OGG/FLAC del path, lo bindea al device (resample por SDL) y
    /// lo deduplica por `key` (la firma del evento) para no reiniciarlo mientras
    /// suena. `offset_seconds` arranca DESDE EL MEDIO del asset (play que
    /// comienza dentro de la ventana de una Secuencia → HD en sync con el
    /// cabezal). Se reapea en tick() como cualquier SFX. No-op sin device.
    /// `gain`: volumen del stream (1 = original) — el slider de la Secuencia.
    /// `preview`: pedido EXPLÍCITO de autoría (botón Reproducir de Mezclar) —
    /// no pertenece al gameplay y el corte de pausa () no lo alcanza.
    /// : devuelve true solo si el stream quedó SONANDO en el device (o el
    /// offset cayó pasado el final, que no es un fallo). false = el original
    /// debe sonar en su lugar.
    bool play_oneshot_asset_file(const std::string& path, uint64_t key,
                                 double offset_seconds = 0.0, float gain = 1.0f,
                                 bool preview = false);

    // ---- Disponibilidad de assets () ------------------------------------

    /// ¿El asset HD de DISCO está decodificado y listo para sonar? Es LA
    /// pregunta previa a silenciar el original: asignado ≠ reproducible.
    /// Sobre una entrada fallida re-verifica el fingerprint (mtime+tamaño) con
    /// rate-limit — un archivo que aparece o se reemplaza vuelve a intentarse
    /// sin reiniciar la sesión, y uno que sigue mal no loguea 60 veces/s.
    bool asset_ready_disk(const std::string& abs_path);
    /// Ídem para un asset DEL PACK. La negativa es permanente: el pack es
    /// inmutable por contenido, lo que falló una vez falla siempre.
    bool asset_ready_pack(AyArchive* pack, const std::string& asset_path);
    /// Diagnóstico para el Lab/telemetría: por qué no está listo ("missing",
    /// "empty", "unsupported", "corrupt") o nullptr si está listo / nunca se
    /// intentó. No dispara IO.
    const char* asset_error_name(const std::string& path) const;

    /// : intentos de arranque de HD que FALLARON con el asset ya listo
    /// (crear/bindear el stream SDL) — la clase rara; los fallos de asset se
    /// ven en asset_error_name y en el fallback del caller.
    uint64_t hd_start_fails() const { return hd_start_fails_; }
    // -- Análisis de nivel () ---------------------------------------------
    //
    // Lo que un autor necesita saber ANTES de publicar: si el asset clipea, si
    // se va a perder en la mezcla, y cuánto habría que corregirlo.
    //
    // Se mide sobre el PCM YA DECODIFICADO —el mismo `wav_cache_` que usa la
    // reproducción— así que analizar un asset que ya sonó no cuesta decode, y
    // analizar uno nuevo lo deja cacheado para cuando suene. Nunca se toca el
    // archivo fuente: todas las correcciones de AYTHER son ganancia en la
    // reproducción, no reescritura.
    /// La struct vive en audio_asset_level.h: la comparten este player (que
    /// mide) y AytherSession (por donde el Lab la consulta), y esos dos headers
    /// no se pueden incluir entre sí.
    using AssetLevel = ayther::AudioAssetLevel;
    /// Mide un asset de disco (WAV/OGG/FLAC). El resultado se cachea por ruta —
    /// recorrer las muestras de un asset largo no es gratis y el Lab lo pregunta
    /// por frame mientras el panel está abierto.
    const AssetLevel& asset_level(const std::string& abs_path);

    /// : la ENVOLVENTE del asset — mínimo y máximo por columna, en -1..1.
    ///
    /// Devuelve `bins` pares (min, max) intercalados: `[min0, max0, min1, …]`.
    /// Min y max y no un solo valor absoluto: una forma de onda dibujada sólo
    /// con el máximo no muestra la asimetría, y ahí es donde se ve el offset de
    /// DC y el recorte de un solo lado — que es la mitad de para qué se mira.
    ///
    /// Se mide sobre el MISMO PCM que la mezcla (S16 44,1 estéreo), igual que
    /// `asset_level`: lo que el autor quiere ver es lo que va a sonar.
    ///
    /// Se cachea por (ruta, bins). El Lab la pide por frame mientras el panel
    /// está abierto y recorrer un asset largo no es gratis.
    const std::vector<float>& asset_waveform(const std::string& abs_path,
                                             uint32_t bins);

    /// Duración en SEGUNDOS de un asset HD de disco (WAV/OGG/FLAC) — decodifica (y
    /// cachea) el archivo y la calcula del PCM. 0 si no se puede leer/decodificar.
    /// NO necesita device de audio abierto (sólo decodifica). Para dimensionar el
    /// span del timeline de Secuencia (el HD puede ser más largo que los eventos).
    double asset_duration_seconds(const std::string& abs_path);
    /// Decodifica un asset de DISCO a PCM S16 estéreo 44100 (el formato del
    /// mixdown del export MP4). Devuelve la cantidad de CUADROS estéreo (out
    /// tiene frames×2 samples); 0 si no se pudo leer/convertir. No necesita
    /// device; el decode crudo se cachea (wav_cache_), la conversión no.
    size_t decode_asset_pcm_s16_44k(const std::string& abs_path,
                                    std::vector<int16_t>& out);
    /// PREWARM: decodifica y cachea un asset HD de disco SIN reproducirlo (no
    /// necesita device). Se llama al abrir el proyecto / setear las subs para que
    /// el PRIMER disparo no pague el decode en pleno playback — el stall hacía
    /// catch-up y se salteaban triggers (reporte 2026-07-23: «la primera vez
    /// algunos sonidos no se escucharon, la segunda sí»). Idempotente (cache).
    void prewarm_asset_file(const std::string& path);
    /// Corta (con el fade-out rápido de tick) los SFX one-shot con esta `key` —
    /// p.ej. el preview HD de la lane de Secuencia al pausar. No-op sin match.
    /// Devuelve true si CORTÓ algo: silenciar a mitad de un asset largo tiene
    /// que poder distinguirse de silenciar entre disparos (), y llamar cada
    /// frame mientras dura el mute es idempotente (el fade ya iniciado no se
    /// reinicia) pero sólo el primero devuelve true.
    bool stop_sfx_by_key(uint64_t key);
    /// Cambia el volumen de un one-shot YA SONANDO. Sin esto, la ganancia sólo
    /// se aplica al crear el stream y arrastrar el slider no se oiría hasta la
    /// próxima re-sincronización — o sea nunca, durante una reproducción
    /// continua, que es justo cuando se quiere ajustar. No toca los que están
    /// en fade-out. Devuelve true si tocó alguno.
    bool set_sfx_gain_by_key(uint64_t key, float gain);
    /// Corta el event-stream de una firma (el camino de play_event_hd, que es
    /// otro stream distinto del one-shot). Devuelve true si cortó algo.
    bool stop_event(uint64_t signature);
    /// True mientras la vista previa one-shot tiene PCM sin drenar (el device la
    /// sigue reproduciendo). El diálogo conmuta Reproducir/Detener con esto, y se
    /// auto-resetea a Reproducir cuando el sonido termina.
    bool preview_playing() const {
        return preview_stream_ && SDL_GetAudioStreamAvailable(preview_stream_) > 0;
    }

    // ---- Accessors ----------------------------------------------------------

    bool              is_open()   const { return device_ != 0; }
    SDL_AudioDeviceID device_id() const { return device_;      }
    /// Streams SDL de one-shot vivos. : tras retirar el camino viejo esto
    /// son SÓLO los previews explícitos de autoría () — los one-shot del
    /// gameplay son voces del mixer y se cuentan con `hd_voice_count()`. Se
    /// deja separado a propósito: sumarlos borraría la distinción que 
    /// necesita afirmar (que el gameplay ya no abre streams propios).
    size_t            sfx_count() const { return sfx_streams_.size(); }
    /// : eventos HD vivos (sustituciones por evento del pack) — el
    /// observable de «¿la ventana lo cortó?» sin oído, hermano de sfx_count().
    /// : sale del mixer — el camino de streams SDL por evento se retiró y
    /// `event_streams_` con él. La cuenta significa lo
    /// mismo que antes (cuántas sustituciones por evento están sonando), así
    /// que los oráculos de  y  siguen midiendo lo que medían.
    size_t            event_count() const { return hd_mixer_.event_voice_count(); }

    // ---- Camino UNIFICADO () --------------------------------------------
    // Los HD del gameplay son voces del HdMixer, sumadas dentro del bloque
    // staged del emulador en su sample exacto: un solo stream, un solo DRC,
    // fase independiente de stalls y catch-up. Los previews EXPLÍCITOS de
    // autoría siguen en streams propios (no son del transporte, ).
    //
    //  retiró el switch `set_unified()` y con él el camino de streams por
    // evento. Era la salida de emergencia mientras el mixer era nuevo; ya no
    // hay dos caminos que mantener ni un A/B que hacer sin recompilar.
    /// Marca que ACÁ empieza el PCM del próximo frame emulado dentro del
    /// bloque staged. La sesión la llama antes de cada run_frame audible: es
    /// lo que coloca un disparo del frame k de un catch-up en SU offset y no
    /// al principio del bloque.
    void mark_frame_boundary() { frame_mark_ = pending_pcm_.size() / 2; }
    /// Cursor ABSOLUTO de la línea de tiempo (cuadros ya flusheados).
    uint64_t timeline_samples() const { return timeline_samples_; }
    size_t   hd_voice_count() const { return hd_mixer_.voice_count(); }
    /// Telemetría  Fase 0: voces arrancadas, atraso acumulado/máximo de
    /// colocación (0 sostenido = la fase es exacta).
    uint64_t hd_voices_started() const { return hd_mixer_.started(); }
    uint64_t hd_mix_skew() const { return hd_mixer_.skew_samples(); }
    uint64_t hd_mix_max_skew() const { return hd_mixer_.max_skew_samples(); }

    // ---- Mute global --------------------------------------------------------
    /// Silencia/restaura TODA la salida poniendo el gain del device a 0/1
    /// (afecta el passthrough del emulador + los SFX HD). Idempotente y seguro
    /// si el device no abrió.
    void set_muted(bool m);
    bool is_muted() const { return muted_; }

    // ---- Dynamic rate control (DRC, v0.10) ----------------------------------
    /// Enable/disable drift compensation on the emulator stream (on by default).
    void  set_drc_enabled(bool on) { drc_enabled_ = on; }
    /// Last frequency ratio applied to emu_stream_ (1.0 = neutral; ~±0.5%).
    float drc_ratio() const { return drc_ratio_; }
    /// : frames de flush con backlog < 1/4 del target (starvation) — telemetria.
    uint64_t starved_frames() const { return starved_frames_; }
    /// : backlog promedio (EMA) en frames del stream del emulador.
    float drc_queue_avg() const { return drc_queue_avg_; }

private:
    // ---- SDL objects --------------------------------------------------------

    SDL_AudioDeviceID device_     = 0;        ///< logical audio device
    SDL_AudioStream*  emu_stream_ = nullptr;  ///< continuous emulator passthrough
    float             game_gain_  = 1.0f;     ///< ducking de la banda sonora (Cinemática)
    /// Stream del sintetizador SoundFont (). SEPARADO del emulador a
    /// propósito: `flush_emulator` saltea el lote entero cuyo hash está
    /// muteado, así que un timbre mezclado ahí se iría al silencio junto con la
    /// voz que viene a reemplazar. Con stream propio, SDL los mezcla en el
    /// device y la mute del original no lo alcanza.
    SDL_AudioStream*  synth_stream_ = nullptr;
    bool              muted_      = false;    ///< gain del device a 0 (mute global)

    // ---- Dynamic rate control -----------------------------------------------
    // Keep emu_stream_'s backlog near a target by nudging its resample ratio
    // ±0.5% (inaudible) so the emulator clock tracks the host device clock —
    // no drift underrun (crackle) / overrun (latency). See ayther-engine.md §6.2.
    bool  drc_enabled_   = true;
    float drc_queue_avg_ = 0.0f;   ///< EMA of queued frames (jitter filter)
    // : diagnostico de starvation — backlog < 1/4 del target = el device va
    // a raspar el fondo (crackle audible). Contador + log rate-limited (1/s).
    /// : target de backlog del stream del emulador (frames @44.1 kHz).
    /// 3072 ≈ 70 ms — colchón que absorbe el dip de una escena densa (~10 ms)
    /// incluso tras un stall, con latencia imperceptible para autoría.
    static constexpr float kDrcTargetFrames = 3072.0f;
    uint64_t starved_frames_    = 0;
    uint64_t last_starve_log_ms_ = 0;
    // : telemetría de la pausa — cuadros descartados por cut_transport_audio
    // (staging + emu + synth) y cuántos cortes descartaron algo.
    uint64_t pause_cut_frames_  = 0;
    uint64_t pause_cuts_        = 0;
    uint64_t last_flush_ms_      = 0;   // : detector de stall (re-cebado)
    // : tee del PCM del emulador a WAV (env AYTHER_AUDIO_DUMP=<ruta>) — lo
    // que se ENCOLA al device (post-mute, pre-DRC). Discriminador objetivo:
    // WAV limpio + oido degradado = problema de ENTREGA (device/pacing);
    // WAV con clicks = problema de CONTENIDO (upstream del encolado).
    void* dump_ = nullptr;            // FILE* (void* para no incluir cstdio aca)
    uint64_t dump_data_bytes_ = 0;
    float drc_ratio_     = 1.0f;   ///< last applied frequency ratio (telemetry)

    struct SfxStream {
        SDL_AudioStream* stream = nullptr;
        uint64_t         hash   = 0;   ///< source audio hash (for dedup)
        /// 0 = sonando normal. != 0 = ms de SDL_GetTicks() en que empezó su
        /// fade-out (lo pisó un disparo nuevo con la MISMA key) — tick() le baja
        /// la ganancia hasta silencio y lo destruye, en vez de dejarlo sonar
        /// entero superpuesto con el nuevo.
        uint64_t         fade_start_ms = 0;
        /// : preview EXPLÍCITO de autoría — el corte de pausa del
        /// transporte (cut_transport_audio) no lo toca.
        bool             preview = false;
    };
    std::vector<SfxStream> sfx_streams_;

    SDL_AudioStream* preview_stream_ = nullptr;  ///< one-shot de la vista previa de audio

    // ---- Deferred emulator passthrough (v0.9.7) -----------------------------

    /// One staged emulator batch: a slice of pending_pcm_ plus its source hash.
    struct PendingBatch {
        uint64_t hash         = 0;  ///< source audio hash (0 = silent / never muted)
        size_t   frame_offset = 0;  ///< start frame within pending_pcm_
        size_t   frames       = 0;  ///< frame count (1 frame = 2 × int16_t)
    };
    std::vector<int16_t>     pending_pcm_;      ///< interleaved S16 stereo, reused
    std::vector<PendingBatch> pending_batches_; ///< this frame's batches, reused

    // ---- Mute set -----------------------------------------------------------

    std::unordered_set<uint64_t> mute_hashes_;      ///< hashes to suppress this frame (HD subs)
    std::unordered_set<uint64_t> user_mute_hashes_; ///< author-muted hashes (persistent)

    // ---- WAV cache ----------------------------------------------------------

    struct WavEntry {
        SDL_AudioSpec        spec = {};
        std::vector<uint8_t> pcm;           ///< decoded PCM bytes (SDL_LoadWAV_IO)
        // : estado del intento. `None` con pcm lleno = listo; cualquier
        // otro valor = cache NEGATIVA con el fingerprint del archivo que falló
        // (0/0 si no existía) — se reintenta solo cuando el fingerprint cambia.
        AssetError err           = AssetError::None;
        int64_t    fp_mtime      = 0;   ///< mtime del intento fallido (disco)
        uint64_t   fp_size       = 0;   ///< tamaño del intento fallido (disco)
        uint64_t   last_check_ms = 0;   ///< rate-limit del re-stat (disco)
    };
    std::unordered_map<std::string, WavEntry> wav_cache_;
    /// : nivel medido por ruta. Se invalida junto con `wav_cache_`.
    std::unordered_map<std::string, AssetLevel> level_cache_;
    /// : envolventes ya calculadas, por (ruta, cantidad de columnas).
    /// La cantidad entra en la clave porque redimensionar el panel cambia las
    /// columnas, y devolver la envolvente de otro ancho dibujaría una forma de
    /// onda que no corresponde al asset que se está mirando.
    std::unordered_map<std::string, std::vector<float>> wave_cache_;
    /// : PCM MIX-READY por asset — el WavEntry convertido UNA vez a
    /// S16 estéreo 44100 (el formato del bloque staged), compartido entre
    /// voces por shared_ptr. La conversión usa SDL_ConvertAudioSamples y no
    /// necesita device.
    std::unordered_map<std::string, HdMixPcm> mix_cache_;
    /// Convierte (y cachea) el PCM de un WavEntry al formato de mezcla.
    /// nullptr si la conversión falla.
    HdMixPcm get_mix_pcm(const WavEntry* wav, const std::string& cache_key);
    /// : cada cuánto como MUCHO se re-statea un asset de disco fallido
    /// para ver si apareció/cambió. Balance: hot-reload perceptible (<1 s)
    /// sin pagar un stat por frame por cada asignación rota.
    static constexpr uint64_t kAssetRecheckMs = 400;
    uint64_t hd_start_fails_ = 0;   ///< : stream-create/bind fallidos

    // ---- Event streams (C-A2) -----------------------------------------------

    /// Stream de un EVENTO sustituido: vive del start_frame al end_frame (si
    /// loopea) o hasta drenar (one-shot). `wav` apunta al cache de assets
    /// (estable: wav_cache_ no borra entradas hasta shutdown).

    // ---- Camino unificado () --------------------------------------------
    HdMixer  hd_mixer_;                  ///< voces HD sumadas al bloque staged
    uint64_t timeline_samples_ = 0;      ///< cuadros ya flusheados (línea de tiempo)
    size_t   frame_mark_       = 0;      ///< offset del frame actual en el staging

    // ---- Helpers ------------------------------------------------------------

    /// Return a cached WavEntry (loading from pack on first access).
    /// Returns nullptr if the asset is missing, corrupt, or pack is null.
    const WavEntry* get_wav(AyArchive* pack, const std::string& asset_path);

    /// Idem pero leyendo de DISCO (asset suelto de autoría) en vez del pack.
    const WavEntry* get_wav_disk(const std::string& abs_path);

    /// Decodifica WAV/OGG/FLAC (por extensión `ext`) desde `raw` a `out`
    /// (spec + pcm). true si decodificó algo usable. Compartido por get_wav (pack)
    /// y get_wav_disk (disco).
    bool decode_audio_bytes(WavEntry& out, const std::vector<uint8_t>& raw,
                            const std::string& ext);
};
