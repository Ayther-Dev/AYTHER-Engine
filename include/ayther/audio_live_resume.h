#pragma once
// ---------------------------------------------------------------------------
// audio_live_resume.h — decisión PURA de reanudación de un reemplazo live
// ().
//
// Una pausa corta físicamente los streams HD (), pero la instancia LÓGICA
// del reemplazo —qué asset, anclado a qué frame, hasta cuándo— sigue viva en
// la sesión. Al reanudar hay que volver a sonar DESDE EL OFFSET que dicta el
// reloj emulado, no desde cero: limpiar los flancos y re-disparar corrige el
// silencio pero introduce desfase, y eso está explícitamente prohibido.
//
// La estrategia elegida es RECREAR el stream desde el offset (no congelar el
// stream físico): es la misma aritmética que ya usa el camino de toma
// ((f - anclaje) / fps), sobrevive a Assets OFF/ON y al cambio de workspace
// con el mismo código, y es compatible con la migración futura al mixer
// unificado () — la instancia lógica no sabe nada de SDL.
//
// Este header es PURO (sin SDL, sin core) para que la decisión sea testeable
// sin sesión ni ROM — mismo criterio que transport_gate.h ().
// ---------------------------------------------------------------------------

#include <cstdint>

namespace ayther {

/// Qué hacer con una instancia live al reanudar el transporte.
enum class LiveResumeAction : uint8_t {
    Restart,    ///< re-crear el stream desde `offset_seconds`
    Finished,   ///< la instancia venció durante la pausa/bypass — descartarla
};

struct LiveResumeDecision {
    LiveResumeAction action         = LiveResumeAction::Finished;
    double           offset_seconds = 0.0;   ///< sólo con Restart
};

constexpr uint64_t kLiveNoCut = UINT64_MAX;   // = drena entero ()

/// ¿Quedó atrás esta instancia? Un loop sin tail muere en end_frame (un loop
/// nunca queda libre — contrato de tick_events); con tail drena hasta
/// cut_frame. Un non-loop manda su cut (kLiveNoCut = sólo lo poda la
/// duración del asset).
constexpr bool live_instance_over(uint64_t frame, uint64_t end_frame,
                                  uint64_t cut_frame, bool looping) noexcept {
    const uint64_t last =
        looping ? (cut_frame == kLiveNoCut ? end_frame : cut_frame)
                : cut_frame;
    return frame > last;
}

/// Decide la reanudación de UNA instancia.
///
/// `frame`         reloj emulado al reanudar (los steps en pausa avanzan y
///                 cuentan: el offset sale de acá, no de un reloj de pared).
/// `start_frame`   anclaje de la instancia (el rising-edge real).
/// `end_frame`     fin de la ventana; kLiveNoCut = one-shot libre (su fin es
///                 la duración del asset).
/// `cut_frame`     corte duro end+tail (); kLiveNoCut = drena entero.
/// `looping`       un loop no tiene «final natural»: vive hasta su ventana
///                 (end, o cut si tiene tail) y su offset conserva la FASE —
///                 el módulo lo aplica el player sobre el PCM decodificado.
/// `fps`           timing del juego (PAL/NTSC); <= 1 usa 60.
/// `asset_seconds` duración del asset si se conoce, 0 = desconocida (la
///                 poda por duración queda en manos del player, que devuelve
///                 éxito-sin-stream si el offset cae pasado el final, ).
inline LiveResumeDecision live_resume_decide(uint64_t frame,
                                             uint64_t start_frame,
                                             uint64_t end_frame,
                                             uint64_t cut_frame,
                                             bool     looping,
                                             double   fps,
                                             double   asset_seconds) noexcept {
    if (live_instance_over(frame, end_frame, cut_frame, looping))
        return {LiveResumeAction::Finished, 0.0};

    const double f = fps > 1.0 ? fps : 60.0;
    const double offset = static_cast<double>(frame - start_frame) / f;

    // Non-loop con duración conocida: pasado el final del PCM no queda nada
    // que reanudar — vencida, aunque su ventana/cut siga formalmente abierta.
    if (!looping && asset_seconds > 0.0 && offset >= asset_seconds)
        return {LiveResumeAction::Finished, 0.0};

    return {LiveResumeAction::Restart, offset};
}

/// Offset de arranque en BYTES dentro del PCM decodificado, alineado a cuadro
/// completo. Para un loop aplica el módulo: la fase se conserva tras
/// cualquier cantidad de pausas. Devuelve un múltiplo de `bytes_per_frame`,
/// siempre < pcm_bytes para loops; para non-loop puede devolver >= pcm_bytes
/// (nada que sonar — el caller decide, ).
inline uint64_t live_resume_offset_bytes(double   offset_seconds,
                                         uint32_t freq,
                                         uint32_t bytes_per_frame,
                                         uint64_t pcm_bytes,
                                         bool     looping) noexcept {
    if (offset_seconds <= 0.0 || freq == 0 || bytes_per_frame == 0)
        return 0;
    uint64_t off = static_cast<uint64_t>(offset_seconds *
                                         static_cast<double>(freq)) *
                   bytes_per_frame;
    if (looping && pcm_bytes >= bytes_per_frame) {
        // Módulo sobre cuadros ENTEROS del asset: pcm_bytes puede no ser
        // múltiplo exacto de bytes_per_frame si el archivo viene raro.
        const uint64_t whole = (pcm_bytes / bytes_per_frame) * bytes_per_frame;
        if (whole > 0) off %= whole;
    }
    return off;
}

}   // namespace ayther
