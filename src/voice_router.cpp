// ---------------------------------------------------------------------------
// voice_router — implementación. Ver voice_router.h para el porqué.
// ---------------------------------------------------------------------------

#include "voice_router.h"

#include "ymfm_opn.h"

#include <algorithm>
#include <cmath>

namespace ayther {
namespace {

// system.h del fork: MCYCLES_PER_LINE 3420.
constexpr uint32_t kMCyclesPerLine = 3420;
// sound.c: YM2612_CLOCK_RATIO (7*6) * 24 — M-cycles por muestra de FM.
constexpr uint32_t kMCyclesPerFmSample = 7 * 6 * 24;   // 1008

constexpr uint32_t mcycles_per_frame(bool pal) {
    return kMCyclesPerLine * (pal ? 313u : 262u);
}

// La interfaz de ymfm sólo necesita existir: todos sus métodos tienen default
// y el YM2612 no usa timers ni IRQ para generar audio.
struct FmIntf : public ymfm::ymfm_interface {};

// A base class is initialized before every base that follows it. ym2612 keeps
// a reference to its interface, so the interface cannot be a Ym2612Mirror
// member: members do not exist until after all bases have been constructed.
struct FmIntfOwner {
    FmIntf intf;
};

}  // namespace

// ---------------------------------------------------------------------------
// Ym2612Mirror — YM2612 con salida POR CANAL.
//
// `m_fm` es protected en ymfm_opn.h, así que esto sale DERIVANDO: la librería
// vendorizada queda intacta y actualizable, sin un solo parche. Replica el
// mixing de ym2612::generate (ymfm_opn.cpp:2380) pero sin sumar los canales.
// ---------------------------------------------------------------------------
class Ym2612Mirror : private FmIntfOwner, public ymfm::ym2612 {
public:
    Ym2612Mirror() : ymfm::ym2612(intf) {}

    void split(output_data out[ChipMirror::kFmChannels]) {
        m_fm.clock(fm_engine::ALL_CHANNELS);
        // Con el DAC prendido, el canal 6 (índice 5) deja de ser FM y pasa a
        // ser la muestra PCM cruda — igual que en el original.
        const int last_fm = m_dac_enable ? 5 : 6;
        for (int ch = 0; ch < ChipMirror::kFmChannels; ++ch) {
            out[ch].clear();
            if (ch < last_fm) {
                output_data tmp;
                m_fm.output(tmp.clear(), 5, 256, 1u << ch);
                out[ch].data[0] = dac_discontinuity(tmp.data[0]);
                out[ch].data[1] = dac_discontinuity(tmp.data[1]);
            }
        }
        if (m_dac_enable) {
            const int32_t dv = dac_discontinuity(int16_t(m_dac_data << 7) >> 7);
            out[5].data[0] = m_fm.regs().ch_output_0(0x102) ? dv : dac_discontinuity(0);
            out[5].data[1] = m_fm.regs().ch_output_1(0x102) ? dv : dac_discontinuity(0);
        }
        // Misma escala que generate(): la salida real es multiplexada, no
        // mezclada, y el 64/65 compensa el ajuste de discontinuidad de arriba.
        for (int ch = 0; ch < ChipMirror::kFmChannels; ++ch) {
            out[ch].data[0] = (out[ch].data[0] * 128) * 64 / (6 * 65);
            out[ch].data[1] = (out[ch].data[1] * 128) * 64 / (6 * 65);
        }
    }

    /// `addr` es el REGISTRO YA LATCHEADO (0x000-0x1FF), no el puerto del bus:
    /// el bit 0x100 elige el banco, o sea los canales 4-6. Se le da a ymfm el
    /// par dirección+dato del banco que toca.
    ///
    /// Lo era hasta el fork `3fc6ee89` (2026-08-11), que dejó el protocolo de
    /// latch del lado del core. Con el `addr & 3` que estaba acá, el registro
    /// 0x28 —key-on— caía en «address port 0» y el dato se interpretaba como
    /// dirección: el espejo no tocaba UNA nota, así que el router entregaba
    /// silencio y con él el Lab entero (2026-08-13, correlación de envolvente
    /// 0,005 contra el 0,99 con el que se aprobó ).
    void write_bus(uint16_t addr, uint8_t data) {
        if (addr & 0x100) {
            write_address_hi(uint8_t(addr & 0xFF));
            write_data_hi(data);
        } else {
            write_address(uint8_t(addr & 0xFF));
            write_data(data);
        }
    }

};

// ---------------------------------------------------------------------------
// ChipMirror
// ---------------------------------------------------------------------------

ChipMirror::ChipMirror() : fm_(std::make_unique<Ym2612Mirror>()) { reset(); }
ChipMirror::~ChipMirror() = default;

void ChipMirror::reset() {
    fm_->reset();
    psg_.reset();
    for (int c = 0; c < kChannels; ++c) {
        buf_[c].clear();
        dc_x1_[c][0] = dc_x1_[c][1] = 0.0f;
        dc_y1_[c][0] = dc_y1_[c][1] = 0.0f;
    }
    generated_ = psg_ticks_ = frame_base_ = 0;
    frame_samples_ = 0;
    dc_primed_     = false;
}

void ChipMirror::set_pal(bool pal) { pal_ = pal; }

double ChipMirror::rate() const {
    // MCLK / 1008. El reloj sale de las líneas por frame × 3420 × fps.
    return (pal_ ? 53203424.0 : 53693175.0) / double(kMCyclesPerFmSample);
}

const float* ChipMirror::channel(int ch) const {
    return (ch >= 0 && ch < kChannels && !buf_[ch].empty()) ? buf_[ch].data() : nullptr;
}

void ChipMirror::gen_until(uint64_t target) {
    ymfm::ym2612::output_data ods[kFmChannels];
    float po[PsgSynth::kChannels];
    while (generated_ < target) {
        fm_->split(ods);
        for (int c = 0; c < kFmChannels; ++c) {
            buf_[c].push_back(float(ods[c].data[0]) / 32768.0f);
            buf_[c].push_back(float(ods[c].data[1]) / 32768.0f);
        }
        // El PSG corre 4,2 ticks por muestra de FM (1008/240). Se promedian los
        // ticks que caen en la muestra en vez de tomar uno: para una onda
        // cuadrada eso es promediar el ciclo de trabajo, que es una primera
        // aproximación honesta a un escalón band-limited. El objetivo exacto se
        // recalcula por muestra ABSOLUTA, así que no deriva.
        const uint64_t psg_target =
            ((generated_ + 1) * kMCyclesPerFmSample) / PsgSynth::kMCyclesPerTick;
        float acc[PsgSynth::kChannels] = {};
        int   cnt = 0;
        while (psg_ticks_ < psg_target) {
            psg_.tick(po);
            for (int c = 0; c < PsgSynth::kChannels; ++c) acc[c] += po[c];
            ++cnt; ++psg_ticks_;
        }
        for (int c = 0; c < PsgSynth::kChannels; ++c) {
            const float v = cnt ? acc[c] / float(cnt) : 0.0f;
            buf_[kFmChannels + c].push_back(v);
            buf_[kFmChannels + c].push_back(v);   // el PSG de la MD es mono
        }
        ++generated_;
    }
}

void ChipMirror::prime_frame(const AytherAudioWrite* w, uint32_t n) {
    // Sólo estado: nada de audio. Es el cebado tras un seek — sin recorrer las
    // escrituras anteriores, el chip no sabe con qué timbre venía sonando cada
    // canal (Fase 0: la correlación de envolvente cae de 0,975 a 0,889).
    for (uint32_t i = 0; i < n; ++i) {
        if (w[i].chip == 0) fm_->write_bus(w[i].addr, w[i].data);
        else                psg_.write(w[i].data);
    }
    const uint32_t mcy = mcycles_per_frame(pal_);
    frame_base_ += mcy;
    // El reloj interno acompaña, o el próximo run_frame generaría el hueco.
    generated_ = frame_base_ / kMCyclesPerFmSample;
    psg_ticks_ = frame_base_ / PsgSynth::kMCyclesPerTick;
}

void ChipMirror::run_frame(const AytherAudioWrite* w, uint32_t n) {
    for (int c = 0; c < kChannels; ++c) buf_[c].clear();
    const uint64_t start = generated_;
    const uint32_t mcy   = mcycles_per_frame(pal_);

    // Cada escritura se aplica en SU posición. Ubicarlas todas al principio del
    // frame amontonaría hasta 16,7 ms de notas en un instante.
    for (uint32_t i = 0; i < n; ++i) {
        gen_until((frame_base_ + w[i].cycle) / kMCyclesPerFmSample);
        if (w[i].chip == 0) fm_->write_bus(w[i].addr, w[i].data);
        else                psg_.write(w[i].data);
    }
    gen_until((frame_base_ + mcy) / kMCyclesPerFmSample);
    frame_base_   += mcy;
    frame_samples_ = size_t(generated_ - start);

    // Bloqueo de continua, y NO es cosmético: dac_discontinuity(0) devuelve +4,
    // así que un canal FM en silencio sale con un escalón de continua (medido en
    // la Fase 0: −51,8 dBFS en un canal sin una sola nota). En el PSG pasa lo
    // mismo con un canal que tiene volumen puesto y nunca recibió frecuencia.
    // Sin quitarlo, tomar o soltar una voz sería un salto de 0 a la continua —
    // o sea un CLIC, justo el defecto que este router viene a eliminar.
    const double R = 1.0 - 2.0 * 3.14159265358979323846 * 5.0 / rate();
    // Cebar el filtro con la PRIMERA muestra, o el escalón de continua entra
    // como un transitorio decayente de ~30 ms en CADA canal mudo: un clic en el
    // arranque y otro después de cada reset. Cebado, un canal que no suena da
    // cero exacto — que es lo que hace que «no se cuela nada» sea comprobable.
    if (!dc_primed_) {
        for (int c = 0; c < kChannels; ++c) {
            if (buf_[c].size() >= 2) {
                dc_x1_[c][0] = buf_[c][0];
                dc_x1_[c][1] = buf_[c][1];
            }
            dc_y1_[c][0] = dc_y1_[c][1] = 0.0f;
        }
        dc_primed_ = true;
    }
    for (int c = 0; c < kChannels; ++c) {
        std::vector<float>& v = buf_[c];
        for (size_t i = 0; i + 1 < v.size(); i += 2) {
            for (int s = 0; s < 2; ++s) {
                const float x = v[i + size_t(s)];
                const float y = float(double(x) - dc_x1_[c][s] + R * dc_y1_[c][s]);
                dc_x1_[c][s] = x;
                dc_y1_[c][s] = y;
                v[i + size_t(s)] = y;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// StreamResampler
//
// Sinc ventaneado de 16 taps. No son los 32-256 del spike porque acá el salto
// es chico (53267 → 44100/48000, un factor de 1,1-1,2) y lo que define la
// calidad del filtro es taps × cutoff: con 16 taps a cutoff 0,83 entran 13
// cruces por cero, de sobra. El spike necesitaba más porque decimaba 5:1.
// ---------------------------------------------------------------------------
namespace {
constexpr int    kRsTaps = 16;
constexpr double kRsPi   = 3.14159265358979323846;
}  // namespace

void StreamResampler::reset() { buf_.clear(); pos_ = 0.0; }

void StreamResampler::set_rates(double src_hz, double dst_hz) {
    step_ = (dst_hz > 0.0) ? (src_hz / dst_hz) : 1.0;
}

void StreamResampler::push(const float* in, size_t frames) {
    if (!in || !frames) return;
    buf_.insert(buf_.end(), in, in + frames * 2);
}

size_t StreamResampler::available() const {
    // Hace falta media ventana de cola por delante para poder filtrar.
    const double have = double(buf_.size() / 2) - double(kRsTaps / 2);
    if (have <= pos_) return 0;
    return size_t((have - pos_) / step_);
}

size_t StreamResampler::pull(float* out, size_t frames) {
    const size_t can = std::min(frames, available());
    const double cutoff = std::min(1.0, 1.0 / step_);   // al bajar de tasa, filtrar más
    const size_t n_in = buf_.size() / 2;
    for (size_t i = 0; i < can; ++i) {
        const long c = long(std::floor(pos_));
        double accL = 0.0, accR = 0.0;
        for (int k = -kRsTaps / 2; k < kRsTaps / 2; ++k) {
            const long idx = c + k;
            if (idx < 0 || size_t(idx) >= n_in) continue;
            const double x  = pos_ - double(idx);
            const double sx = x * cutoff;
            const double s  = (std::fabs(sx) < 1e-9) ? 1.0
                                                     : std::sin(kRsPi * sx) / (kRsPi * sx);
            const double wa = (x + kRsTaps / 2.0) / double(kRsTaps);
            const double w  = 0.42 - 0.5 * std::cos(2.0 * kRsPi * wa)
                                   + 0.08 * std::cos(4.0 * kRsPi * wa);
            const double h  = s * w * cutoff;
            accL += buf_[size_t(idx) * 2]     * h;
            accR += buf_[size_t(idx) * 2 + 1] * h;
        }
        out[i * 2]     = float(accL);
        out[i * 2 + 1] = float(accR);
        pos_ += step_;
    }
    // Descartar lo consumido, dejando media ventana de historia para el próximo
    // bloque — sin eso el filtro arrancaría cojo en cada juntura.
    const long keep_from = long(std::floor(pos_)) - kRsTaps / 2;
    if (keep_from > 0) {
        buf_.erase(buf_.begin(), buf_.begin() + size_t(keep_from) * 2);
        pos_ -= double(keep_from);
    }
    return can;
}

// ---------------------------------------------------------------------------
// CopySource
// ---------------------------------------------------------------------------

void CopySource::render(const float* chan, float* out, size_t n) {
    if (!chan || !out) return;
    for (size_t i = 0; i < n * 2; ++i) out[i] += chan[i];
}

// : la copia con un factor. El espejo ya entrega el PCM del canal por
// separado, así que esto es el render de CopySource con una multiplicación —
// que es exactamente lo que lo hace barato.
void GainSource::render(const float* chan, float* out, size_t n) {
    if (!chan || !out) return;
    // Por RENDER: el usuario mueve el slider con la música sonando y tiene que
    // oírlo ahora, no en el próximo key-on (que en un loop puede no llegar).
    const float g = gain_ ? *gain_ : 1.0f;
    if (g == 1.0f) { for (size_t i = 0; i < n * 2; ++i) out[i] += chan[i]; return; }
    for (size_t i = 0; i < n * 2; ++i) out[i] += chan[i] * g;
}

// ---------------------------------------------------------------------------
// ChannelRouter
// ---------------------------------------------------------------------------

ChannelRouter::ChannelRouter() { reset(); }

void ChannelRouter::reset() {
    mirror_.reset();
    for (int i = 0; i < kVoices; ++i) voice_[i] = &copy_;
    fm_key_ = 0;
    for (int i = 0; i < 4; ++i) psg_att_[i] = 0xF;
    psg_latch_ = 3;
    ++stats_.resets;
}

void ChannelRouter::scan_keys(const AytherAudioWrite* w, uint32_t n,
                              std::vector<KeyEvent>& out) {
    out.clear();
    for (uint32_t i = 0; i < n; ++i) {
        const AytherAudioWrite& e = w[i];
        if (e.chip == 0) {
            // El key-on del YM2612 vive en el registro 0x28 del banco 0. El
            // core entrega el registro ya latcheado (ver write_bus): replicar
            // acá el protocolo de puertos hacía que ningún key-on se
            // reconociera, y sin key-on el router no tiene voces que rutear.
            if (e.addr != 0x28) continue;
            int ch = e.data & 0x03;
            if (ch == 3) continue;              // valor inválido
            if (e.data & 0x04) ch += 3;         // 4/5/6 → canales 3/4/5
            const bool on = (e.data & 0xF0) != 0;   // algún operador encendido
            const uint8_t bit = uint8_t(1u << ch);
            if (on == ((fm_key_ & bit) != 0)) continue;   // sin cambio
            if (on) fm_key_ |= bit; else fm_key_ &= uint8_t(~bit);
            out.push_back({ 0, uint8_t(ch), on, e.cycle });
        } else {
            // El PSG no tiene key-on: una voz «arranca» cuando deja de estar
            // atenuada al máximo y «para» cuando vuelve a 0xF.
            const uint8_t d = e.data;
            int index;
            if (d & 0x80) psg_latch_ = index = (d >> 4) & 0x07;
            else          index = psg_latch_;
            if ((index & 1) == 0) continue;     // registro de frecuencia
            const int ch = index >> 1;
            const uint8_t att = uint8_t(d & 0x0F);
            const bool was_on = psg_att_[ch] != 0xF;
            const bool now_on = att != 0xF;
            psg_att_[ch] = att;
            if (was_on == now_on) continue;
            out.push_back({ 1, uint8_t(ch), now_on, e.cycle });
        }
    }
}

void ChannelRouter::tick(const AytherAudioWrite* w, uint32_t n, uint32_t frame,
                         std::vector<float>& out) {
    ++stats_.ticks;

    // El espejo primero: los key-on se resuelven CONTRA su PCM, y hace falta
    // saber cuántas muestras produjo este frame antes de repartir tramos.
    mirror_.run_frame(w, n);
    const size_t ns = mirror_.frame_samples();
    out.assign(ns * 2, 0.0f);
    if (ns == 0) return;

    std::vector<KeyEvent> keys;
    scan_keys(w, n, keys);

    const uint32_t mcy = mcycles_per_frame(mirror_.pal());
    auto sample_at = [&](uint32_t cycle) -> size_t {
        // Proporcional dentro del frame: el espejo ya ubicó las escrituras por
        // su cycle, así que acá alcanza con el prorrateo para partir el bloque.
        const uint64_t s = uint64_t(cycle) * uint64_t(ns) / uint64_t(mcy);
        return size_t(std::min<uint64_t>(s, ns));
    };

    for (int slot = 0; slot < kVoices; ++slot) {
        const uint8_t chip = slot < ChipMirror::kFmChannels ? 0 : 1;
        const uint8_t chan = uint8_t(slot < ChipMirror::kFmChannels
                                         ? slot : slot - ChipMirror::kFmChannels);
        const float* src = mirror_.channel(slot);
        if (!src) continue;

        size_t pos = 0;
        for (const KeyEvent& k : keys) {
            if (k.chip != chip || k.channel != chan) continue;
            const size_t at = sample_at(k.cycle);
            if (at > pos) {
                voice_[slot]->render(src + pos * 2, out.data() + pos * 2, at - pos);
                pos = at;
            }
            if (k.on) {
                ++stats_.key_ons;
                voice_[slot]->key_off();          // la voz anterior suelta el canal
                const VoiceContext ctx{ chip, chan, frame, k.cycle };
                IVoiceSource* v = policy_ ? policy_->choose(ctx) : nullptr;
                // Sin política, o si la fuente elegida no puede arrancar, suena
                // el original. Es la degradación correcta: se oye de más, no de
                // menos — un silencio por error es mucho peor que una copia.
                if (!v || !v->begin(ctx)) v = &copy_;
                else if (v != &copy_)     ++stats_.substituted;
                voice_[slot] = v;
            } else {
                ++stats_.key_offs;
                voice_[slot]->key_off();
            }
        }
        if (pos < ns)
            voice_[slot]->render(src + pos * 2, out.data() + pos * 2, ns - pos);

        // Una fuente que terminó su cola devuelve el canal a la copia: si no, el
        // canal quedaría mudo hasta el próximo key-on.
        if (voice_[slot]->finished()) voice_[slot] = &copy_;
    }
}

}  // namespace ayther
