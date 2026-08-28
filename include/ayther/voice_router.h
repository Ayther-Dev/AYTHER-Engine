// ---------------------------------------------------------------------------
// voice_router — el router de canales por voz (, Fase 2).
//
// LA INVERSIÓN. Hasta ahora la sustitución era SUSTRACTIVA: el chip sonaba
// entero y se tapaban canales con una máscara derivada de VENTANAS de eventos.
// Una ventana es un MODELO del sonido; el chip ES el sonido, así que todo
// instante que la ventana no cubriera pero el chip siguiera sonando era una
// fuga —  (huecos entre nota y nota) y  (juntura entre Secuencias) son
// el mismo defecto visto en dos lugares.
//
// Acá el default se da vuelta: el chip queda MUDO y todo lo que se oye lo
// produce este router. Una voz se toma el canal DESDE EL KEY-ON DEL PROPIO CHIP
// hasta el fin de su cola. No hay ventana en la que equivocarse.
//
// LAS DOS PIEZAS
//   ChipMirror  — un YM2612 (ymfm) y un SN76489 (PsgSynth) alimentados con el
//                 mismo log de escrituras que recibe el core, produciendo los
//                 10 canales POR SEPARADO. Es el sustrato: corre siempre,
//                 tomen o no las voces su salida.
//   ChannelRouter — 10 slots. En cada key-on le pregunta a la política qué debe
//                 sonar y apunta el slot a esa fuente.
//
// POR QUÉ EL ESPEJO CORRE SIEMPRE: su estado de registros ES la identidad del
// sonido. Si se apagara mientras una voz está sustituida, al volver no sabría
// con qué timbre sonaba — medido en la Fase 0: arrancar el sintetizador sin el
// estado previo baja la correlación de envolvente de 0,975 a 0,889.
// ---------------------------------------------------------------------------

#pragma once

#include "ayther_core_ffi.h"
#include "psg_synth.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace ymfm { class ym2612; }

namespace ayther {

class Ym2612Mirror;   // detalle de implementación: ym2612 con salida por canal

// ---------------------------------------------------------------------------
// ChipMirror
// ---------------------------------------------------------------------------
class ChipMirror {
public:
    static constexpr int kFmChannels  = 6;
    static constexpr int kPsgChannels = 4;
    static constexpr int kChannels    = kFmChannels + kPsgChannels;

    ChipMirror();
    ~ChipMirror();

    /// Corta todo y vuelve a cero. Para los seeks y los cortes de escena.
    /// OJO: esto TIRA el estado de registros, así que después hay que cebarlo
    /// (prime) o el timbre sale mal — no es como el panic de un sampler.
    void reset();

    /// PAL cambia las líneas por frame y con eso los M-cycles por frame.
    void set_pal(bool pal);
    bool pal() const { return pal_; }

    /// Consume las escrituras de un frame y genera su PCM por canal.
    /// Las escrituras se ubican por su `cycle`, así que una nota que arranca a
    /// mitad de frame arranca a mitad de frame — no en el borde.
    void run_frame(const AytherAudioWrite* w, uint32_t n);

    /// Igual que run_frame pero SIN generar audio: sólo hace avanzar el estado
    /// de los registros. Es el cebado tras un seek — hay que recorrer las
    /// escrituras anteriores para saber con qué timbre venía sonando cada canal.
    void prime_frame(const AytherAudioWrite* w, uint32_t n);

    /// PCM del canal aislado del último run_frame: interleaved L,R, ya SIN
    /// CONTINUA (ver dc_block_ en el .cpp — un canal en silencio no queda en
    /// cero y el escalón sería un clic al tomar o soltar la voz).
    const float* channel(int ch) const;

    /// Muestras estéreo que produjo el último run_frame.
    size_t frame_samples() const { return frame_samples_; }

    /// Tasa interna = MCLK/1008 = 53267,03 Hz en NTSC. Es la tasa NATIVA del
    /// YM2612; el PSG (que corre 4,2× más rápido) se promedia hasta acá.
    double rate() const;

private:
    void gen_until(uint64_t target_sample);

    std::unique_ptr<Ym2612Mirror> fm_;
    PsgSynth                      psg_;
    std::vector<float>            buf_[kChannels];
    float                         dc_x1_[kChannels][2] = {};
    float                         dc_y1_[kChannels][2] = {};
    bool                          dc_primed_ = false;
    uint64_t                      generated_    = 0;   // muestras de FM totales
    uint64_t                      psg_ticks_    = 0;   // ticks de PSG totales
    uint64_t                      frame_base_   = 0;   // M-cycles al inicio del frame
    size_t                        frame_samples_ = 0;
    bool                          pal_           = false;
};

// ---------------------------------------------------------------------------
// StreamResampler — de la tasa del chip a la del device, conservando la fase.
//
// Hace falta con estado y no un resample por bloque porque los dos extremos no
// están en fase y el bloque cambia de tamaño frame a frame: el Lab no corre a
// 16,7 ms por frame (el probe mide 20-33), así que el catch-up entrega a veces
// varios frames juntos. Reiniciar la fase en cada bloque metería un salto en
// cada juntura — un chasquido por frame.
// ---------------------------------------------------------------------------
class StreamResampler {
public:
    void reset();
    void set_rates(double src_hz, double dst_hz);

    /// Encola muestras estéreo interleaved a la tasa de ORIGEN.
    void push(const float* in, size_t frames);

    /// Saca hasta `frames` muestras estéreo a la tasa de DESTINO. Devuelve
    /// cuántas pudo entregar — menos que las pedidas significa que falta
    /// entrada, y el que llama decide si rellena con silencio o espera.
    size_t pull(float* out, size_t frames);

    /// Muestras de destino que ya se pueden sacar con lo encolado.
    size_t available() const;

private:
    std::vector<float> buf_;      // interleaved a la tasa de origen
    double             pos_  = 0.0;   // posición de lectura, en muestras de origen
    double             step_ = 1.0;   // src/dst
};

// ---------------------------------------------------------------------------
// Una voz: un canal tomado entre un key-on y el fin de su cola.
// ---------------------------------------------------------------------------
struct VoiceContext {
    uint8_t  chip    = 0;   ///< 0 = YM2612 (FM) · 1 = SN76489 (PSG)
    uint8_t  channel = 0;   ///< FM 0-5 · PSG 0-3
    uint32_t frame   = 0;   ///< frame del key-on
    uint32_t cycle   = 0;   ///< M-cycle DENTRO del frame: la posición exacta
};

/// Qué suena en un canal. `chan` es el PCM del canal aislado del espejo para
/// este tramo; una fuente puede copiarlo, ignorarlo o mezclarse con él.
class IVoiceSource {
public:
    virtual ~IVoiceSource() = default;

    /// El chip disparó esta voz. Devolver false = no puedo sonar → cae a copia,
    /// que es la degradación correcta (se oye el original, no un silencio).
    virtual bool begin(const VoiceContext& ctx) = 0;

    /// Suma `n` muestras estéreo de `chan` en `out`. Los dos son interleaved.
    virtual void render(const float* chan, float* out, size_t n) = 0;

    /// El chip soltó la tecla. No es el final: falta la cola.
    virtual void key_off() = 0;

    /// Terminó la cola. Una copia nunca termina —el espejo ya modela su propio
    /// release— así que devuelve false y el slot se libera recién en el próximo
    /// key-on.
    virtual bool finished() const = 0;
};

/// Copia el canal aislado tal cual. Es el default y la degradación de todo lo
/// demás: si algo no puede sonar, se oye el original.
class CopySource final : public IVoiceSource {
public:
    bool begin(const VoiceContext&) override { return true; }
    void render(const float* chan, float* out, size_t n) override;
    void key_off() override {}
    bool finished() const override { return false; }
};

/// No suena nada. Es lo que hace que «no se cuele nada» sea estructural: no es
/// un mute que hay que acertar a tiempo, es una fuente que no emite.
class SilentSource final : public IVoiceSource {
public:
    bool begin(const VoiceContext&) override { return true; }
    void render(const float*, float*, size_t) override {}
    void key_off() override {}
    bool finished() const override { return false; }
};

/// : copia el canal ESCALADO. Es lo que faltaba entre «copiar» y «callar»
/// para que bajarle el volumen a un bus alcance también al audio ORIGINAL —
/// hasta ahora la música del juego o sonaba entera o se callaba.
///
/// EL FACTOR SE LEE POR RENDER, NO SE COPIA EN `begin()`.
///
/// Es la trampa que este repo ya pagó dos veces: la ganancia por Secuencia y el
/// volumen del bus, las dos aplicadas sólo al arrancar el sonido. En un loop
/// musical «al arrancar» es NUNCA — el usuario mueve el slider y no pasa nada
/// hasta la próxima vez que la música empiece de cero, que puede ser jamás.
///
/// Por eso guarda un PUNTERO al factor y no su valor: el dueño del volumen es
/// la política, que lo cambia cuando el usuario mueve el control, y la fuente
/// lo lee cada vez que rellena.
class GainSource final : public IVoiceSource {
public:
    /// `gain` tiene que sobrevivir a la fuente. En la práctica apunta al array
    /// de ganancias por bus de la sesión, que vive tanto como ella.
    explicit GainSource(const float* gain) : gain_(gain) {}
    bool begin(const VoiceContext&) override { return true; }
    void render(const float* chan, float* out, size_t n) override;
    void key_off() override {}
    bool finished() const override { return false; }
private:
    const float* gain_ = nullptr;
};

/// Quién decide. La implementación real (Fase 4) traduce las asignaciones que
/// ya existen —inst_assign del SF2, audio_event_assign, los mutes— a fuentes.
class VoicePolicy {
public:
    virtual ~VoicePolicy() = default;
    /// Qué suena en este canal desde este key-on. nullptr = copia.
    /// El puntero NO se adueña: la política es dueña de sus fuentes.
    virtual IVoiceSource* choose(const VoiceContext& ctx) = 0;
};

// ---------------------------------------------------------------------------
// ChannelRouter
// ---------------------------------------------------------------------------
class ChannelRouter {
public:
    static constexpr int kVoices = ChipMirror::kChannels;

    ChannelRouter();

    void set_policy(VoicePolicy* p) { policy_ = p; }
    ChipMirror&       mirror()       { return mirror_; }
    const ChipMirror& mirror() const { return mirror_; }

    /// Corta todo. Deja los slots en copia y limpia el espejo.
    void reset();

    /// Un frame: consume el log, resuelve los key-on/key-off en su posición
    /// exacta, y deja la mezcla en `out` (interleaved, `frame_samples()`
    /// muestras). `out` se PISA, no se acumula.
    void tick(const AytherAudioWrite* w, uint32_t n, uint32_t frame,
              std::vector<float>& out);

    /// Telemetría — el precedente de synth_stats: sin esto, diagnosticar de
    /// oído cuesta varias vueltas.
    struct Stats {
        uint64_t ticks = 0, key_ons = 0, key_offs = 0;
        uint64_t substituted = 0;   // key-ons que NO cayeron en copia
        uint64_t resets = 0;
    };
    const Stats& stats() const { return stats_; }

private:
    /// Un key-on/key-off detectado en las escrituras, con su posición.
    struct KeyEvent { uint8_t chip, channel; bool on; uint32_t cycle; };

    void scan_keys(const AytherAudioWrite* w, uint32_t n,
                   std::vector<KeyEvent>& out);

    ChipMirror   mirror_;
    VoicePolicy* policy_ = nullptr;
    CopySource   copy_;
    SilentSource silent_;

    IVoiceSource* voice_[kVoices] = {};
    uint8_t       fm_key_ = 0;              // estado key-on de los 6 canales FM
    uint8_t       psg_att_[4] = { 0xF, 0xF, 0xF, 0xF };   // atenuación actual
    int           psg_latch_ = 3;
    Stats         stats_;
};

}  // namespace ayther
