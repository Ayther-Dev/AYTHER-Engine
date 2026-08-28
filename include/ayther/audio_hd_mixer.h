#pragma once
// ---------------------------------------------------------------------------
// audio_hd_mixer.h — mezclador de voces HD sobre la línea de tiempo de
// MUESTRAS del stream principal ().
//
// EL PROBLEMA QUE CIERRA. Los reemplazos HD corrían en streams SDL propios:
// arrancaban «ya» respecto del reloj de pared, mientras el original viajaba
// por emu_stream_ con ~70 ms de colchón DRC — o sea que la fase entre original
// y HD dependía del backlog, de los stalls y del tamaño del catch-up. Acá cada
// voz se COLOCA en un sample absoluto de la línea de tiempo del bloque staged
// y se suma DENTRO de ese bloque: un disparo en el frame N cae en el mismo
// sample ejecutando 1×1 o catch-up 16, y todo —original, router, HD— cruza el
// MISMO DRC/backlog. La pausa corta un solo stream y corta todo ().
//
// QUÉ ES ESTE MÓDULO. Sólo la mezcla: voces con PCM ya decodificado y
// convertido (S16 estéreo 44100 — lo garantiza el cache del AudioPlayer),
// colocación por muestra, loops con fase, ganancia, fade de corte y el
// contrato de vida por frame de  (end + tail). NO toca SDL: la mezcla es
// una función pura sobre un buffer, y por eso el oráculo de identidad
// 1×1-vs-catch-up puede ser exacto, byte a byte, sin device.
//
// El ciclo de vida por FRAME (end_frame/cut_frame) se mantiene en frames a
// propósito: es el mismo contrato que tick_events y que las ventanas de la
// sesión (/) — la conversión frame→sample vive en UN solo lugar (la
// colocación del arranque), no repartida por todos los sweeps.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <memory>
#include <vector>

/// PCM mix-ready compartido: S16 estéreo intercalado a 44100 Hz. Compartido
/// (shared_ptr) porque N voces del mismo asset no deben duplicar el decode, y
/// porque la voz debe sobrevivir a una invalidación del cache sin colgar.
using HdMixPcm = std::shared_ptr<const std::vector<int16_t>>;

class HdMixer {
public:
    /// Fade de corte, en CUADROS a 44100 (~60 ms — el mismo criterio audible
    /// que el fade de stop_sfx_by_key en el camino de streams).
    static constexpr uint32_t kFadeFrames = 2646;

    struct Voice {
        uint64_t key          = 0;
        HdMixPcm pcm;                       ///< S16 estéreo 44100 (mix-ready)
        uint64_t start_sample = 0;          ///< sample ABSOLUTO de arranque
        size_t   pos          = 0;          ///< cursor en CUADROS dentro del pcm
        float    gain         = 1.0f;
        bool     looping      = false;
        bool     event        = false;      ///< contrato de play_event_hd ()
        uint64_t end_frame    = UINT64_MAX; ///< fin de ventana (dominio FRAME)
        uint64_t cut_frame    = UINT64_MAX; ///< end + tail (); MAX = drena
        /// > 0 = cuadros de fade restantes (la voz muere al llegar a 0).
        uint32_t fade_left    = 0;
        /// Cuadros con los que ARRANCO el fade en curso — el denominador de la
        /// rampa. Sin esto, un fade autorado de 30 000 cuadros se dividiria por
        /// `kFadeFrames` y saldria clavado en ganancia > 1 hasta el final.
        uint32_t fade_span    = kFadeFrames;
        /// : politica de fin FADE_OUT, en cuadros a 44100. 0 = sin fade
        /// (hard_cut o tail, segun ). La rampa arranca al pasar
        /// `end_frame` y llega a silencio `fade_frames` despues — que es lo
        /// que el autor pidio: «termina en silencio a los N frames del limite».
        uint32_t fade_frames  = 0;
        /// : REGIÓN de loop dentro del asset, en CUADROS. `loop_end == 0`
        /// = sin región: se repite el asset entero, que es lo único que había.
        ///
        /// Sólo aplica cuando `looping`: una región en un one-shot no querría
        /// decir nada, y recortarlo por ella sería cortar el sonido a la mitad.
        size_t   loop_begin   = 0;
        size_t   loop_end     = 0;
        /// Telemetría: cuántas muestras tarde llegó el arranque (0 = en fase).
        uint64_t late_samples = 0;
    };

    /// Arranca (o re-dispara) una voz. `start_sample` = posición ABSOLUTA en
    /// la línea de tiempo del stream donde cae su primer cuadro — el caller la
    /// calcula como (timeline + offset del frame actual dentro del bloque
    /// staged), que es lo que hace la colocación inmune al catch-up.
    /// `offset_frames` arranca desde el medio del asset (reanudación ,
    /// disparo tardío): para un loop entra por el módulo (fase conservada).
    /// Un re-disparo de la misma key hace fade de la voz anterior (el
    /// contrato del retrigger de siempre). Devuelve false sin PCM utilizable.
    bool start(uint64_t key, HdMixPcm pcm, uint64_t start_sample,
               uint64_t offset_frames, float gain, bool looping, bool event,
               uint64_t end_frame, uint64_t cut_frame,
               uint32_t fade_frames = 0,
               // : región de loop en CUADROS (0,0 = el asset entero).
               size_t loop_begin = 0, size_t loop_end = 0) {
        if (!pcm || pcm->size() < 2) return false;
        stop(key);   // retrigger: la anterior sale con fade, no superpuesta
        Voice v;
        v.key = key;
        v.start_sample = start_sample;
        v.gain = gain;
        v.looping = looping;
        v.event = event;
        v.end_frame = end_frame;
        v.cut_frame = cut_frame;
        v.fade_frames = fade_frames;
        const size_t frames = pcm->size() / 2;
        // La región se SANEA acá y no al mezclar: un `loop_end` mayor que el
        // asset o invertido leería fuera del buffer, y comprobarlo por muestra
        // sería pagar el chequeo 44 100 veces por segundo para algo que no
        // cambia en toda la vida de la voz.
        if (loop_end > frames) loop_end = frames;
        if (loop_begin >= loop_end) { loop_begin = 0; loop_end = 0; }
        v.loop_begin = loop_begin;
        v.loop_end   = loop_end;
        v.pos = looping ? static_cast<size_t>(offset_frames % frames)
                        : static_cast<size_t>(offset_frames);
        // Reanudar () dentro de un loop con región: si el offset cae fuera
        // del ciclo, entra por su fase DENTRO de la región. Dejarlo antes del
        // inicio haría que la reanudación se comiera la intro otra vez.
        if (v.loop_end && v.pos >= v.loop_end) {
            const size_t span = v.loop_end - v.loop_begin;
            v.pos = v.loop_begin + ((v.pos - v.loop_begin) % span);
        }
        v.pcm = std::move(pcm);
        // Non-loop con el offset pasado el final: nada que mezclar — éxito
        // sin voz, el mismo contrato que el camino de streams ().
        if (!v.looping && v.pos >= frames) return true;
        voices_.push_back(std::move(v));
        ++started_;
        return true;
    }

    /// Corte con fade (~60 ms) de todas las voces de una key. true si alcanzó
    /// a alguna que no estuviera ya en fade — el mismo contrato observable que
    /// stop_sfx_by_key (: distinguir «corté a mitad» de «no había nada»).
    bool stop(uint64_t key) {
        bool cut = false;
        for (Voice& v : voices_)
            if (v.key == key && v.fade_left == 0) {
                v.fade_left = kFadeFrames;
                v.fade_span = kFadeFrames;   // el de corte, no el autorado
                cut = true;
            }
        return cut;
    }

    /// Corte DURO inmediato (pausa  / seek): sin fade, sin residuo — el
    /// staging se descarta entero en esos caminos y la voz no debe drenar.
    void cut_all() { voices_.clear(); }

    /// Corte duro filtrado por clase: los caminos de la sesión distinguen
    /// one-shots (stop_all_sfx) de event-streams (stop_all_events) y el modo
    /// unificado tiene que respetar esa frontera — un seek que invalida los
    /// eventos de la toma no debe callar un one-shot ajeno.
    void cut_all_of(bool event) {
        for (size_t i = voices_.size(); i-- > 0; )
            if (voices_[i].event == event)
                voices_.erase(voices_.begin() + static_cast<std::ptrdiff_t>(i));
    }

    bool stop_hard(uint64_t key) {
        const size_t n = voices_.size();
        for (size_t i = voices_.size(); i-- > 0; )
            if (voices_[i].key == key)
                voices_.erase(voices_.begin() + static_cast<std::ptrdiff_t>(i));
        return voices_.size() != n;
    }

    /// Ganancia en vivo de una key (slider de Secuencia sonando).
    bool set_gain(uint64_t key, float gain) {
        bool any = false;
        for (Voice& v : voices_)
            if (v.key == key && v.fade_left == 0) { v.gain = gain; any = true; }
        return any;
    }

    /// El contrato de vida por FRAME — espejo exacto de tick_events ():
    /// pasado cut_frame la voz muere aunque le quede PCM; un loop pasado su
    /// end_frame muere (sin tail) o deja de loopear y drena hasta cut (tail).
    void tick_frame(uint64_t frame) {
        for (size_t i = voices_.size(); i-- > 0; ) {
            Voice& v = voices_[i];
            // : FADE_OUT. Al pasar el límite la voz no muere de golpe:
            // arranca una rampa de `fade_frames` cuadros y se apaga sola al
            // llegar a silencio. Es una política de fin ALTERNATIVA a tail —
            // no se acumulan— y por eso se evalúa ANTES que cut_frame: el
            // corte duro de la ventana no debe interrumpir la rampa que el
            // autor pidió. Un fade ya en curso no se re-arranca.
            if (v.fade_frames && frame > v.end_frame) {
                if (v.fade_left == 0) {
                    v.fade_left = v.fade_frames;
                    v.fade_span = v.fade_frames;
                    v.looping   = false;   // deja de re-alimentarse: se apaga
                }
                continue;
            }
            if (frame > v.cut_frame) {
                voices_.erase(voices_.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            if (v.looping && frame > v.end_frame) {
                if (v.cut_frame == UINT64_MAX || v.cut_frame <= v.end_frame)
                    voices_.erase(voices_.begin() + static_cast<std::ptrdiff_t>(i));
                else
                    v.looping = false;   // tail: drena el resto hasta cut
            }
        }
    }

    /// SUMA las voces sobre `out` (S16 estéreo intercalado, `frames` cuadros),
    /// que representa las muestras [block_start, block_start + frames) de la
    /// línea de tiempo. Colocación por muestra: una voz cuyo start cae DENTRO
    /// del bloque entra en su offset exacto — eso es lo que hace idéntico el
    /// resultado entre 1×1 y catch-up. Una voz programada para un bloque que
    /// ya pasó entra al principio y registra su atraso (telemetría skew).
    void mix_into(int16_t* out, size_t frames, uint64_t block_start) {
        if (!out || frames == 0) return;
        for (size_t i = voices_.size(); i-- > 0; ) {
            Voice& v = voices_[i];
            if (v.start_sample >= block_start + frames) continue;   // futuro
            size_t at = 0;   // offset en CUADROS dentro del bloque
            if (v.start_sample > block_start) {
                at = static_cast<size_t>(v.start_sample - block_start);
            } else if (v.start_sample < block_start && v.pos == 0 &&
                       v.late_samples == 0 && !v.looping) {
                // Primer bloque de una voz que llegó TARDE (p.ej. programada
                // durante un stall): registrar el atraso una vez.
                v.late_samples = block_start - v.start_sample;
                skew_samples_ += v.late_samples;
                if (v.late_samples > max_skew_) max_skew_ = v.late_samples;
            }
            const std::vector<int16_t>& pcm = *v.pcm;
            const size_t pcm_frames = pcm.size() / 2;
            bool done = false;
            // : dónde termina el ciclo y a dónde vuelve. Sin región es el
            // asset entero, que es el comportamiento de siempre.
            const size_t cycle_end = (v.looping && v.loop_end) ? v.loop_end : pcm_frames;
            const size_t cycle_beg = (v.looping && v.loop_end) ? v.loop_begin : 0;
            for (size_t f = at; f < frames && !done; ++f) {
                if (v.pos >= cycle_end) {
                    if (!v.looping) { done = true; break; }
                    // Se vuelve al INICIO DE LA REGIÓN, no al del archivo: eso
                    // es lo que deja que un tema tenga intro y después cicle
                    // sin exportar dos archivos.
                    v.pos = cycle_beg;
                }
                float g = v.gain;
                if (v.fade_left > 0) {
                    g *= static_cast<float>(v.fade_left) /
                         static_cast<float>(v.fade_span ? v.fade_span : 1);
                    if (--v.fade_left == 0) done = true;
                }
                const size_t s = v.pos * 2;
                const size_t o = f * 2;
                for (int c = 0; c < 2; ++c) {
                    const int32_t mixed = static_cast<int32_t>(out[o + c]) +
                        static_cast<int32_t>(
                            static_cast<float>(pcm[s + c]) * g);
                    out[o + c] = static_cast<int16_t>(
                        mixed > 32767 ? 32767 : (mixed < -32768 ? -32768 : mixed));
                }
                ++v.pos;
            }
            if (done || (!v.looping && v.pos >= pcm_frames &&
                         v.fade_left == 0 && v.start_sample <= block_start))
                voices_.erase(voices_.begin() + static_cast<std::ptrdiff_t>(i));
        }
        mixed_samples_ += frames;
    }

    size_t voice_count() const { return voices_.size(); }
    size_t event_voice_count() const {
        size_t n = 0;
        for (const Voice& v : voices_) n += v.event ? 1 : 0;
        return n;
    }

    // ---- Telemetría ( Fase 0) ------------------------------------------
    uint64_t started() const { return started_; }
    uint64_t mixed_samples() const { return mixed_samples_; }
    /// Muestras de atraso acumuladas de voces que llegaron a un bloque ya
    /// pasado (0 sostenido = la colocación funciona; crece = hay un camino
    /// que programa contra una línea de tiempo que ya se flusheó).
    uint64_t skew_samples() const { return skew_samples_; }
    uint64_t max_skew_samples() const { return max_skew_; }

private:
    std::vector<Voice> voices_;
    uint64_t started_       = 0;
    uint64_t mixed_samples_ = 0;
    uint64_t skew_samples_  = 0;
    uint64_t max_skew_      = 0;
};
