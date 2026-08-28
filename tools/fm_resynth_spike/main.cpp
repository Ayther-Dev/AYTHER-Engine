// ---------------------------------------------------------------------------
// fm_resynth_spike — el GATE de la Fase 0 del router de canales por voz (#327).
//
// PREGUNTA: si alimentamos un sintetizador FM PROPIO (ymfm, BSD-3) con el log
// de escrituras que el fork ya expone (id 0x109), ¿suena como el chip del core?
//
// De eso depende toda la reingeniería. El plan es dejar el chip MUDO al 100% y
// que TODO el audio lo produzca un router de 10 voces; si la re-síntesis no
// pasa, hay que caer a la otra vía (tap por canal adentro del fork, bit-exacto
// pero toca el core GPL) y eso cambia el plan entero.
//
// LO QUE MIDE
//   R1  · fidelidad de la MEZCLA — nulo con ganancia óptima tras alinear el
//         desfase, correlación de envolvente, brillo (ZCR), y los WAV.
//   R1b · fidelidad POR CANAL — la prueba que de verdad importa, porque es la
//         feature: el core corre con un solo canal FM audible y ymfm rinde ese
//         mismo canal aislado. Es el CopySource del router, medido.
//   R2  · jitter — el `cycle` de cada escritura no es replay-estable
//         (audio_chip_spike ya lo reporta). ¿Offset constante o jitter real?
//   R3  · costo — cuánto tarda re-sintetizar un frame contra los 16,7 ms.
//   R4  · DAC — escrituras al canal 6 (tambores PCM) y si el log satura:
//         sound.c:58 TRUNCA EN SILENCIO, y para identificar da igual pero para
//         re-sintetizar una escritura perdida es un sonido equivocado.
//
// EL VEREDICTO ES EL OÍDO. Dos emulaciones distintas del YM2612 no cancelan
// nunca a nivel de muestra; el nulo orienta, no decide.
//
// POR QUÉ FM CONTRA FM: la corrida de referencia mutea el PSG en el core
// (bits 6-9) para que la comparación sea del mismo chip.
//
// TASA: la ruta MAME OPN2 del core corre a MCLK/1008 = 53267,03 Hz
// (sound.c:360, fm_cycles_ratio = 7*6*24) y ymfm con clock de entrada
// 7670453 Hz genera a input/144 = la MISMA tasa. Los dos motores FM coinciden
// muestra a muestra; el único resample es el de salida, para comparar contra el
// PCM que el core ya entrega resampleado.
//
//   fm_resynth_spike <core.dll> <rom> [--warmup N] [--frames N] [--out DIR]
// ---------------------------------------------------------------------------

// retro_runner.h arrastra windows.h, que define min/max como MACROS y rompe
// std::min/std::max. Mismo tropiezo que ya costó una vuelta en pose_resolve.h.
#define NOMINMAX

#include "libretro_host/retro_runner.h"
#include "psg_synth.h"
#include "ymfm_opn.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static_assert(sizeof(RetroRunner::AudioWrite) == 8,
              "AudioWrite debe ser 8 bytes (ABI idéntico al AytherAudioWrite del fork)");

static constexpr double kPi = 3.14159265358979323846;

// Reloj del YM2612 en una Mega Drive NTSC: MCLK/7. Su tasa de salida es
// clock/144 = 53267,03 Hz, idéntica a la del core (ver cabecera).
static constexpr uint32_t kYm2612Clock        = 7670453;
// M-cycles por muestra de FM — sound.c: YM2612_CLOCK_RATIO (7*6) * 24.
static constexpr uint32_t kMCyclesPerFmSample = 7 * 6 * 24;   // 1008
static constexpr uint32_t kMCyclesPerLine     = 3420;         // system.h
static constexpr int      kFmChannels         = 6;

static constexpr uint16_t BTN_START = 1u << 3;
static constexpr uint16_t BTN_RIGHT = 1u << 7;
static constexpr uint16_t BTN_A     = 1u << 8;

// Mismo input scripted que audio_chip_spike: lo que haga en el juego da igual,
// lo que importa es que todas las corridas usen la MISMA secuencia.
static uint16_t scripted_input(int frame) {
    uint16_t b = BTN_RIGHT;
    if (frame % 24 < 4) b |= BTN_A;
    return b;
}

// ---------------------------------------------------------------------------
// ymfm: la interfaz sólo necesita existir — todos sus métodos tienen default
// (ymfm.h:500-552). El YM2612 no usa timers ni IRQ para generar audio.
// ---------------------------------------------------------------------------
struct FmIntf : public ymfm::ymfm_interface {};

// ---------------------------------------------------------------------------
// YM2612 con salida POR CANAL.
//
// `m_fm` es protected en ymfm_opn.h:772, así que esto sale DERIVANDO — sin
// tocar una línea de la librería vendorizada, que es lo que la mantiene
// actualizable. Replica el mixing de ym2612::generate (ymfm_opn.cpp:2380) pero
// sin sumar los canales.
//
// Esto es exactamente el CopySource del router: el canal aislado, para poder
// reemplazar uno y dejar los otros cinco intactos.
// ---------------------------------------------------------------------------
class Ym2612Split : public ymfm::ym2612 {
public:
    explicit Ym2612Split(ymfm::ymfm_interface& intf) : ymfm::ym2612(intf) {}

    void generate_split(output_data out[kFmChannels]) {
        m_fm.clock(fm_engine::ALL_CHANNELS);
        // Con el DAC prendido el canal 6 (índice 5) deja de ser FM y pasa a ser
        // la muestra PCM cruda — igual que en el original.
        const int last_fm = m_dac_enable ? 5 : 6;
        for (int ch = 0; ch < kFmChannels; ++ch) {
            out[ch].clear();
            if (ch < last_fm) {
                output_data tmp;
                m_fm.output(tmp.clear(), 5, 256, 1u << ch);
                out[ch].data[0] = dac_discontinuity(tmp.data[0]);
                out[ch].data[1] = dac_discontinuity(tmp.data[1]);
            }
        }
        if (m_dac_enable) {
            const int32_t dacval = dac_discontinuity(int16_t(m_dac_data << 7) >> 7);
            out[5].data[0] = m_fm.regs().ch_output_0(0x102) ? dacval : dac_discontinuity(0);
            out[5].data[1] = m_fm.regs().ch_output_1(0x102) ? dacval : dac_discontinuity(0);
        }
        // Misma escala que generate(): la salida real es multiplexada, no
        // mezclada, y el 64/65 compensa el ajuste de discontinuidad de arriba.
        for (int ch = 0; ch < kFmChannels; ++ch) {
            out[ch].data[0] = (out[ch].data[0] * 128) * 64 / (6 * 65);
            out[ch].data[1] = (out[ch].data[1] * 128) * 64 / (6 * 65);
        }
    }
};

// ---------------------------------------------------------------------------
// WAV 16-bit estéreo. El oráculo de este proyecto es el oído, así que el spike
// no termina en un número: termina en archivos que se pueden escuchar.
// ---------------------------------------------------------------------------
static bool write_wav(const std::string& path, const std::vector<float>& pcm,
                      uint32_t rate) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const uint32_t data_sz = static_cast<uint32_t>(pcm.size()) * 2;
    const uint32_t riff_sz = 36 + data_sz;
    const uint16_t channels = 2, bits = 16;
    const uint32_t byte_rate = rate * channels * bits / 8;
    const uint16_t block = channels * bits / 8;
    const uint32_t fmt_sz = 16; const uint16_t fmt_tag = 1;
    std::fwrite("RIFF", 1, 4, f);       std::fwrite(&riff_sz, 4, 1, f);
    std::fwrite("WAVEfmt ", 1, 8, f);   std::fwrite(&fmt_sz, 4, 1, f);
    std::fwrite(&fmt_tag, 2, 1, f);     std::fwrite(&channels, 2, 1, f);
    std::fwrite(&rate, 4, 1, f);        std::fwrite(&byte_rate, 4, 1, f);
    std::fwrite(&block, 2, 1, f);       std::fwrite(&bits, 2, 1, f);
    std::fwrite("data", 1, 4, f);       std::fwrite(&data_sz, 4, 1, f);
    for (float v : pcm) {
        const int16_t o = static_cast<int16_t>(
            std::lrint(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
        std::fwrite(&o, 2, 1, f);
    }
    std::fclose(f);
    return true;
}

// ---------------------------------------------------------------------------
// Resample estéreo por sinc ventaneado (Blackman, 32 taps).
//
// Es el ÚNICO paso que introduce error propio en la comparación, y va dicho en
// el reporte: el residuo del nulo lo incluye. No se puede evitar — el core
// entrega su PCM ya resampleado a 44,1 kHz y el chip corre a 53,3 kHz.
// ---------------------------------------------------------------------------
static std::vector<float> resample_stereo(const std::vector<float>& in,
                                          double src_hz, double dst_hz) {
    const size_t n_in = in.size() / 2;
    if (n_in == 0) return {};
    const double ratio = dst_hz / src_hz;
    const size_t n_out = static_cast<size_t>(static_cast<double>(n_in) * ratio);
    std::vector<float> out(n_out * 2, 0.0f);
    // Al BAJAR de tasa hay que bajar el corte del filtro o entra aliasing.
    const double cutoff = (src_hz > dst_hz) ? (dst_hz / src_hz) : 1.0;
    // Y los taps tienen que crecer con la decimación: lo que define la calidad
    // del filtro es cuántos CRUCES POR CERO del sinc entran, o sea taps*cutoff.
    // Con 32 fijos, el FM (cutoff 0,83) queda bien pero el PSG —que decima 5:1,
    // cutoff 0,20— se queda con 6 cruces y deja pasar aliasing. Eso destruye la
    // correlación de muestra sin tocar la envolvente, que es exactamente lo que
    // se vio: PSG con envolvente 0,97 y forma de onda 0,31.
    const int kTaps = std::clamp(int(32.0 / cutoff + 0.5), 32, 256) & ~1;

    for (size_t i = 0; i < n_out; ++i) {
        const double t = static_cast<double>(i) / ratio;
        const long   c = static_cast<long>(std::floor(t));
        double accL = 0.0, accR = 0.0;
        for (int k = -kTaps / 2; k < kTaps / 2; ++k) {
            const long idx = c + k;
            if (idx < 0 || static_cast<size_t>(idx) >= n_in) continue;
            const double x  = t - static_cast<double>(idx);
            const double sx = x * cutoff;
            const double s  = (std::fabs(sx) < 1e-9) ? 1.0
                                                     : std::sin(kPi * sx) / (kPi * sx);
            const double wa = (x + kTaps / 2.0) / static_cast<double>(kTaps);
            const double w  = 0.42 - 0.5 * std::cos(2.0 * kPi * wa)
                                   + 0.08 * std::cos(4.0 * kPi * wa);
            const double h  = s * w * cutoff;
            accL += in[static_cast<size_t>(idx) * 2]     * h;
            accR += in[static_cast<size_t>(idx) * 2 + 1] * h;
        }
        out[i * 2]     = static_cast<float>(accL);
        out[i * 2 + 1] = static_cast<float>(accR);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Bloqueo de continua.
//
// HALLAZGO DEL SPIKE, y va a valer para el router: un canal aislado NO queda en
// cero cuando calla. dac_discontinuity(0) devuelve +4 (ymfm_opn.h:765), así que
// el silencio de una voz es un ESCALÓN DE CONTINUA — medido, −51,8 dBFS en un
// canal sin una sola nota.
//
// En la mezcla no molesta porque el chip real lo resuelve con el capacitor de
// salida, pero AISLANDO canales importa: arrancar o parar una voz sería un salto
// de 0 a la continua, o sea un CLIC. Exactamente el defecto que el router viene
// a eliminar. Todo CopySource tiene que pasar por acá.
// ---------------------------------------------------------------------------
static void dc_block(std::vector<float>& v, double rate) {
    const double R = 1.0 - 2.0 * kPi * 5.0 / rate;   // pasa-altos de ~5 Hz
    double x1L = 0, y1L = 0, x1R = 0, y1R = 0;
    for (size_t i = 0; i + 1 < v.size(); i += 2) {
        const double xL = v[i], xR = v[i + 1];
        y1L = xL - x1L + R * y1L; x1L = xL; v[i]     = float(y1L);
        y1R = xR - x1R + R * y1R; x1R = xR; v[i + 1] = float(y1R);
    }
}

static double rms(const std::vector<float>& v, size_t from = 0, size_t to = 0) {
    if (to == 0) to = v.size();
    if (to <= from) return 0.0;
    double acc = 0.0;
    for (size_t i = from; i < to; ++i) acc += double(v[i]) * double(v[i]);
    return std::sqrt(acc / double(to - from));
}
static double dbfs(double x) { return x > 1e-12 ? 20.0 * std::log10(x) : -240.0; }

// ---------------------------------------------------------------------------
// Comparación core-contra-resíntesis.
// ---------------------------------------------------------------------------
struct Cmp {
    long   lag       = 0;      // desfase corregido, en muestras
    double lag_corr  = 0.0;    // correlación de muestra en ese desfase
    double gain      = 0.0;    // ganancia óptima
    double null_db   = 0.0;    // residuo relativo al original
    double env_corr  = 0.0;    // correlación de envolvente por frame
    double core_dbfs = -240.0;
    double zcr_core = 0.0, zcr_syn = 0.0;
    size_t n = 0;
    std::vector<float> aligned_core, aligned_syn, null_pcm;
};

static Cmp compare(std::vector<float> a,             // PCM del core
                   const std::vector<float>& b_fm,   // resíntesis a fm_rate
                   double core_rate, double fm_rate, double fps) {
    Cmp r;
    std::vector<float> b = resample_stereo(b_fm, fm_rate, core_rate);
    size_t n = std::min(a.size(), b.size());
    a.resize(n); b.resize(n);
    if (n == 0) return r;
    // Los dos lados sin continua, o la comparación mide el escalón del DAC y no
    // la forma de onda (ver dc_block).
    dc_block(a, core_rate);
    dc_block(b, core_rate);

    // DESFASE. Antes de medir nada hay que alinear: el PCM del core sale del
    // blip buffer con su propia latencia. Sin esto el nulo mide el corrimiento
    // y no la fidelidad — un desfase de UNA muestra ya destruye la cancelación
    // de un agudo, y el número diría «no sirve» sobre una re-síntesis perfecta.
    // La ventana de búsqueda va donde AMBOS tienen energía, no en el centro a
    // ciegas: si al canal le toca un silencio justo ahí, la correlación da 0 y
    // el desfase queda sin corregir — con lo que el nulo mide el corrimiento.
    // (Pasó: los canales 3 y 4 daban correlación 0,0000 con nivel normal.)
    constexpr size_t kWin = 100000;
    size_t win_start = 0;
    {
        double best_e = -1.0;
        for (size_t s = 0; s + kWin <= n; s += kWin / 2) {
            const double ea = rms(a, s, s + kWin), eb = rms(b, s, s + kWin);
            const double e  = std::min(ea, eb);      // el mínimo: los DOS deben sonar
            if (e > best_e) { best_e = e; win_start = s; }
        }
    }
    auto corr_at = [&](long lag) {
        const size_t start = win_start;
        const size_t stop  = std::min(n, start + kWin);
        double num = 0.0, da = 0.0, db = 0.0;
        for (size_t i = start; i < stop; i += 2) {      // sólo canal L
            const long j = long(i) + lag * 2;
            if (j < 0 || size_t(j) >= n) continue;
            num += double(a[i]) * b[size_t(j)];
            da  += double(a[i]) * a[i];
            db  += double(b[size_t(j)]) * b[size_t(j)];
        }
        return (da > 1e-18 && db > 1e-18) ? num / std::sqrt(da * db) : 0.0;
    };
    r.lag_corr = -2.0;
    for (long l = -4410; l <= 4410; l += 16) {
        const double c = corr_at(l);
        if (c > r.lag_corr) { r.lag_corr = c; r.lag = l; }
    }
    for (long l = r.lag - 24; l <= r.lag + 24; ++l) {
        const double c = corr_at(l);
        if (c > r.lag_corr) { r.lag_corr = c; r.lag = l; }
    }
    if (r.lag > 0)      b.erase(b.begin(), b.begin() + size_t(r.lag) * 2);
    else if (r.lag < 0) a.erase(a.begin(), a.begin() + size_t(-r.lag) * 2);
    n = std::min(a.size(), b.size());
    a.resize(n); b.resize(n);
    r.n = n;

    // Ganancia óptima: el core aplica su preamp y su DAC ladder, ymfm no. La
    // pregunta no es si los niveles coinciden sino si la FORMA coincide.
    double num = 0.0, den = 0.0;
    for (size_t i = 0; i < n; ++i) { num += double(a[i]) * b[i];
                                     den += double(b[i]) * b[i]; }
    r.gain = den > 1e-12 ? num / den : 0.0;

    r.null_pcm.resize(n);
    for (size_t i = 0; i < n; ++i) r.null_pcm[i] = a[i] - float(r.gain * b[i]);
    const double rc = rms(a);
    r.core_dbfs = dbfs(rc);
    r.null_db   = dbfs(rms(r.null_pcm)) - dbfs(rc);

    // Envolvente por frame: el detector de errores de TIEMPO y de NOTA. Una
    // nota que falta, o que llega tarde, hunde esto aunque el nulo se vea
    // razonable por la energía promedio.
    const size_t win = std::max<size_t>(2, size_t(core_rate / fps) * 2);
    std::vector<double> ea, eb;
    for (size_t i = 0; i + win <= n; i += win) {
        ea.push_back(rms(a, i, i + win));
        eb.push_back(rms(b, i, i + win));
    }
    double ma = 0, mb = 0;
    for (size_t i = 0; i < ea.size(); ++i) { ma += ea[i]; mb += eb[i]; }
    if (!ea.empty()) { ma /= double(ea.size()); mb /= double(ea.size()); }
    double cov = 0, va = 0, vb = 0;
    for (size_t i = 0; i < ea.size(); ++i) {
        cov += (ea[i] - ma) * (eb[i] - mb);
        va  += (ea[i] - ma) * (ea[i] - ma);
        vb  += (eb[i] - mb) * (eb[i] - mb);
    }
    r.env_corr = (va > 1e-18 && vb > 1e-18) ? cov / std::sqrt(va * vb) : 0.0;

    // Brillo: cruces por cero. Proxy crudo pero real del contenido de agudos —
    // si la re-síntesis sonara apagada o metálica, esto se separa.
    auto zcr = [](const std::vector<float>& v) {
        size_t z = 0;
        for (size_t i = 2; i < v.size(); i += 2)
            if ((v[i] >= 0.0f) != (v[i - 2] >= 0.0f)) ++z;
        return v.size() ? double(z) / (double(v.size()) / 2.0) : 0.0;
    };
    r.zcr_core = zcr(a);
    r.zcr_syn  = zcr(b);

    r.aligned_core = std::move(a);
    r.aligned_syn.resize(n);
    for (size_t i = 0; i < n; ++i) r.aligned_syn[i] = float(r.gain * b[i]);
    return r;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
            "fm_resynth_spike — ¿la re-síntesis FM propia suena como el core? (#327, Fase 0)\n"
            "usage: %s <core.dll> <rom> [--warmup N] [--frames N] [--out DIR]\n",
            argv[0]);
        return 2;
    }
    const std::string core = argv[1];
    const std::string rom  = argv[2];
    int warmup = 200, frames = 400;
    std::string outdir = ".";
    for (int i = 3; i + 1 < argc; i += 2) {
        if      (std::strcmp(argv[i], "--warmup") == 0) warmup = std::atoi(argv[i + 1]);
        else if (std::strcmp(argv[i], "--frames") == 0) frames = std::atoi(argv[i + 1]);
        else if (std::strcmp(argv[i], "--out")    == 0) outdir = argv[i + 1];
    }

    RetroRunner runner;
    if (!runner.init(core, rom)) {
        std::fprintf(stderr, "[spike] init falló (¿path de core/rom?)\n");
        return 1;
    }

    // E-2 (#398): sin suscripción el fork no instrumenta NADA, y este spike
    // medía un log vacío como si el espejo fuera el que no suena.
    std::fprintf(stdout, "[spike] suscripciones: 0x%08X\n",
                 runner.subscribe_all_supported());

    std::vector<float> core_pcm;
    std::vector<uint32_t> core_frame_n;
    uint32_t pending = 0;
    bool collect = false;
    runner.set_audio_callback([&](const int16_t* data, size_t n) -> size_t {
        if (collect && data) {
            for (size_t i = 0; i < n * 2; ++i)
                core_pcm.push_back(float(data[i]) / 32768.0f);
            pending += static_cast<uint32_t>(n);
        }
        return n;
    });

    std::fprintf(stdout, "[spike] core fps=%.4f\n", runner.fps());
    runner.run_frame();
    if (runner.audio_writes() == nullptr) {
        std::fprintf(stderr,
            "[spike] el core NO expone el log de escrituras (id 0x109).\n"
            "        ¿DLL sin instrumentar? Usá el fork _vram.\n");
        return 3;
    }

    // El warm-up TAMBIÉN se registra. Sin esto la re-síntesis arranca desde un
    // chip en reset mientras el core llega con N frames de estado encima: los
    // patches de la música que ya suena se escribieron ANTES de la ventana, y
    // sin ellos ymfm toca las notas correctas con el timbre equivocado.
    //
    // No es un detalle del arnés: es la misma restricción que va a tener el
    // router. Un sintetizador propio necesita el ESTADO de registros, así que
    // tras un seek hay que reconstruirlo — no alcanza con mirar el log de ahí
    // en adelante. (Medido: sin priming la correlación de envolvente baja de
    // 0,975 a 0,889.)
    std::vector<std::vector<RetroRunner::AudioWrite>> log(size_t(warmup) + size_t(frames));
    for (int i = 0; i < warmup; ++i) {
        runner.set_input(0, (i % 64 < 3) ? BTN_START : 0);
        runner.run_frame();
        const uint32_t cnt = runner.audio_write_count();
        const RetroRunner::AudioWrite* w = runner.audio_writes();
        if (w && cnt) log[size_t(i)].assign(w, w + cnt);
    }

    std::vector<uint8_t> S;
    if (!runner.serialize(S)) {
        std::fprintf(stderr, "[spike] el core no soporta savestate (hace falta para R1b/R2).\n");
        return 3;
    }

    // -- Corrida de referencia: FM solo (PSG muteado) ------------------------
    const uint16_t kMutePsg = 0x3C0;               // bits 6-9
    const uint16_t kMuteAll = 0x3FF;
    collect = true;
    for (int i = 0; i < frames; ++i) {
        runner.set_audio_mute_v1(kMutePsg);
        runner.set_input(0, scripted_input(i));
        pending = 0;
        runner.run_frame();
        core_frame_n.push_back(pending);
        const uint32_t cnt = runner.audio_write_count();
        const RetroRunner::AudioWrite* w = runner.audio_writes();
        if (w && cnt) log[size_t(warmup) + size_t(i)].assign(w, w + cnt);
    }
    collect = false;

    // Tasa de salida del core: MEDIDA, no asumida (no hay accessor, y asumir
    // 44100 sería una fuente de error silenciosa en el resample).
    uint64_t total_core = 0;
    for (uint32_t n : core_frame_n) total_core += n;
    const double core_rate = double(total_core) * runner.fps() / double(frames);

    // -- R4: DAC y saturación del log (sólo la ventana medida) ---------------
    uint64_t dac_writes = 0, fm_writes = 0, psg_writes = 0;
    uint32_t max_per_frame = 0; int saturated_frames = 0;
    for (size_t fi = size_t(warmup); fi < log.size(); ++fi) {
        for (const auto& e : log[fi]) {
            if (e.chip == RetroRunner::kAudioChipFM) {
                ++fm_writes;
                if (e.addr == 0x2A) ++dac_writes;   // registro ya latcheado
            } else ++psg_writes;
        }
        max_per_frame = std::max(max_per_frame, static_cast<uint32_t>(log[fi].size()));
        if (log[fi].size() >= 8192) ++saturated_frames;
    }

    // -- Re-síntesis ---------------------------------------------------------
    // Ubicación EXACTA de cada escritura: el `cycle` es en M-cycles dentro del
    // frame y una muestra de FM son 1008. Se acumula el conteo GLOBAL para que
    // no derive — dividir por frame y redondear iría sumando error, que es el
    // bug que ya costó una medición en mute_silence_probe.
    const uint32_t lines = (runner.fps() > 1.0 && runner.fps() < 55.0) ? 313 : 262;
    const uint64_t mcy_frame = uint64_t(kMCyclesPerLine) * lines;
    const double   fm_rate = double(kYm2612Clock) / 144.0;

    FmIntf intf_mix, intf_split;
    ymfm::ym2612 chip_mix(intf_mix);
    Ym2612Split  chip_split(intf_split);
    chip_mix.reset();
    chip_split.reset();

    std::vector<float> syn_mix;
    std::vector<std::vector<float>> syn_ch(kFmChannels);
    uint64_t gen_mix = 0, gen_split = 0, syn_skip = 0;
    ymfm::ym2612::output_data od, ods[kFmChannels];

    // `addr` = REGISTRO ya latcheado (bit 0x100 = banco 1, canales 4-6). El
    // core resuelve el latch desde el fork 3fc6ee89; con el `addr & 3` que
    // estaba acá el espejo no tocaba una nota y el spike medía 0,005 de
    // correlación como si el problema fuera ymfm.
    auto feed = [&](const RetroRunner::AudioWrite& e, ymfm::ym2612& c) {
        if (e.addr & 0x100) {
            c.write_address_hi(uint8_t(e.addr & 0xFF));
            c.write_data_hi(e.data);
        } else {
            c.write_address(uint8_t(e.addr & 0xFF));
            c.write_data(e.data);
        }
    };

    const auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < log.size(); ++i) {
        if (i == size_t(warmup)) syn_skip = gen_mix;
        const uint64_t base = uint64_t(i) * mcy_frame;
        for (const auto& e : log[i]) {
            if (e.chip != RetroRunner::kAudioChipFM) continue;   // el PSG va aparte
            const uint64_t target = (base + e.cycle) / kMCyclesPerFmSample;
            while (gen_mix < target) {
                chip_mix.generate(&od, 1);
                syn_mix.push_back(float(od.data[0]) / 32768.0f);
                syn_mix.push_back(float(od.data[1]) / 32768.0f);
                ++gen_mix;
            }
            while (gen_split < target) {
                chip_split.generate_split(ods);
                for (int c = 0; c < kFmChannels; ++c) {
                    syn_ch[c].push_back(float(ods[c].data[0]) / 32768.0f);
                    syn_ch[c].push_back(float(ods[c].data[1]) / 32768.0f);
                }
                ++gen_split;
            }
            feed(e, chip_mix);
            feed(e, chip_split);
        }
        const uint64_t end = (base + mcy_frame) / kMCyclesPerFmSample;
        while (gen_mix < end) {
            chip_mix.generate(&od, 1);
            syn_mix.push_back(float(od.data[0]) / 32768.0f);
            syn_mix.push_back(float(od.data[1]) / 32768.0f);
            ++gen_mix;
        }
        while (gen_split < end) {
            chip_split.generate_split(ods);
            for (int c = 0; c < kFmChannels; ++c) {
                syn_ch[c].push_back(float(ods[c].data[0]) / 32768.0f);
                syn_ch[c].push_back(float(ods[c].data[1]) / 32768.0f);
            }
            ++gen_split;
        }
    }
    const double synth_ms = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - t0).count();

    // Descartar el warm-up: se generó sólo para dejar el chip con el estado de
    // registros correcto al entrar a la ventana medida.
    syn_mix.erase(syn_mix.begin(), syn_mix.begin() + size_t(syn_skip) * 2);
    for (auto& v : syn_ch) v.erase(v.begin(), v.begin() + size_t(syn_skip) * 2);

    const Cmp mix = compare(core_pcm, syn_mix, core_rate, fm_rate, runner.fps());

    // -- R1b: por canal ------------------------------------------------------
    // El core corre con UN SOLO canal FM audible y ymfm rinde ese mismo canal
    // aislado. Es la feature del router medida de frente, no por el agregado.
    struct ChRes { Cmp cmp; double core_lvl = -240.0; double syn_lvl = -240.0; };
    std::vector<ChRes> chres(kFmChannels);
    for (int ch = 0; ch < kFmChannels; ++ch) {
        if (!runner.unserialize(S)) break;
        core_pcm.clear();
        collect = true;
        const uint16_t mask = uint16_t(kMuteAll & ~(1u << ch));
        for (int i = 0; i < frames; ++i) {
            runner.set_audio_mute_v1(mask);
            runner.set_input(0, scripted_input(i));
            runner.run_frame();
        }
        collect = false;
        chres[ch].core_lvl = dbfs(rms(core_pcm));
        chres[ch].syn_lvl  = dbfs(rms(syn_ch[ch]));
        chres[ch].cmp = compare(core_pcm, syn_ch[ch], core_rate, fm_rate, runner.fps());
    }

    // -- R1c: el PSG propio --------------------------------------------------
    // ymfm no hace SN76489, así que este chip es nuestro (src/psg_synth.cpp)
    // y hay que validarlo igual que el FM: el core corriendo SÓLO PSG (FM
    // muteado) contra nuestra síntesis desde el mismo log.
    //
    // Va la mezcla Y canal por canal: el canal suelto es el que dice DÓNDE se
    // separa el modelo, que en la mezcla queda enmascarado.
    constexpr int kPsgCh = ayther::PsgSynth::kChannels;
    const double psg_rate = double(mcy_frame) * runner.fps()
                          / double(ayther::PsgSynth::kMCyclesPerTick);
    ayther::PsgSynth psg;
    std::vector<float> psg_mix;
    std::vector<std::vector<float>> psg_chb(kPsgCh);
    uint64_t psg_gen = 0, psg_skip = 0;
    float po[kPsgCh];
    auto psg_until = [&](uint64_t target) {
        while (psg_gen < target) {
            psg.tick(po);
            float m = 0.0f;
            for (int c = 0; c < kPsgCh; ++c) {
                m += po[c];
                psg_chb[c].push_back(po[c]); psg_chb[c].push_back(po[c]);
            }
            psg_mix.push_back(m); psg_mix.push_back(m);   // el PSG de la MD es mono
            ++psg_gen;
        }
    };
    for (size_t i = 0; i < log.size(); ++i) {
        if (i == size_t(warmup)) psg_skip = psg_gen;
        const uint64_t base = uint64_t(i) * mcy_frame;
        for (const auto& e : log[i]) {
            if (e.chip != RetroRunner::kAudioChipPSG) continue;
            psg_until((base + e.cycle) / ayther::PsgSynth::kMCyclesPerTick);
            psg.write(e.data);
        }
        psg_until((base + mcy_frame) / ayther::PsgSynth::kMCyclesPerTick);
    }
    psg_mix.erase(psg_mix.begin(), psg_mix.begin() + size_t(psg_skip) * 2);
    for (auto& v : psg_chb) v.erase(v.begin(), v.begin() + size_t(psg_skip) * 2);

    // Corre el core con la máscara pedida y devuelve su PCM.
    auto run_masked = [&](uint16_t mask) {
        core_pcm.clear();
        if (!runner.unserialize(S)) return false;
        collect = true;
        for (int i = 0; i < frames; ++i) {
            runner.set_audio_mute_v1(mask);
            runner.set_input(0, scripted_input(i));
            runner.run_frame();
        }
        collect = false;
        return true;
    };

    Cmp psg_cmp; double psg_core_lvl = -240.0;
    if (run_masked(0x3F)) {                            // mutear FM → queda el PSG
        psg_core_lvl = dbfs(rms(core_pcm));
        psg_cmp = compare(core_pcm, psg_mix, core_rate, psg_rate, runner.fps());
    }
    std::vector<ChRes> psgres(kPsgCh);
    if (psg_core_lvl > -80.0) {
        for (int c = 0; c < kPsgCh; ++c) {
            if (!run_masked(uint16_t(kMuteAll & ~(1u << (6 + c))))) break;
            psgres[c].core_lvl = dbfs(rms(core_pcm));
            psgres[c].syn_lvl  = dbfs(rms(psg_chb[c]));
            psgres[c].cmp = compare(core_pcm, psg_chb[c], core_rate, psg_rate, runner.fps());
        }
    }

    // -- R2: ¿el cycle es offset constante o jitter real? --------------------
    uint64_t cmp_writes = 0, differing = 0; long max_delta = 0;
    int frames_const_offset = 0, frames_compared = 0, frames_seq_differs = 0;
    if (runner.unserialize(S)) {
        for (int i = 0; i < frames; ++i) {
            runner.set_audio_mute_v1(kMutePsg);
            runner.set_input(0, scripted_input(i));
            runner.run_frame();
            const uint32_t cnt = runner.audio_write_count();
            const RetroRunner::AudioWrite* w = runner.audio_writes();
            const auto& ref = log[size_t(warmup) + size_t(i)];
            if (cnt != ref.size()) { ++frames_seq_differs; continue; }
            if (!cnt) continue;
            long dmin = 0, dmax = 0; bool first = true, same = true;
            for (uint32_t k = 0; k < cnt; ++k) {
                if (w[k].chip != ref[k].chip || w[k].addr != ref[k].addr ||
                    w[k].data != ref[k].data) { same = false; break; }
                const long d = long(w[k].cycle) - long(ref[k].cycle);
                ++cmp_writes;
                if (d != 0) ++differing;
                max_delta = std::max(max_delta, std::labs(d));
                if (first) { dmin = dmax = d; first = false; }
                else { dmin = std::min(dmin, d); dmax = std::max(dmax, d); }
            }
            if (!same) { ++frames_seq_differs; continue; }
            ++frames_compared;
            if (dmin == dmax) ++frames_const_offset;
        }
    }
    runner.set_audio_mute_v1(0);

    // -- Reporte -------------------------------------------------------------
    const uint32_t wav_rate = uint32_t(core_rate + 0.5);
    write_wav(outdir + "/core.wav",    mix.aligned_core, wav_rate);
    write_wav(outdir + "/resynth.wav", mix.aligned_syn,  wav_rate);
    write_wav(outdir + "/null.wav",    mix.null_pcm,     wav_rate);
    for (int ch = 0; ch < kFmChannels; ++ch) {
        if (chres[ch].core_lvl < -80.0) continue;      // canal sin actividad
        char nm[64];
        std::snprintf(nm, sizeof nm, "/ch%d_core.wav", ch);
        write_wav(outdir + nm, chres[ch].cmp.aligned_core, wav_rate);
        std::snprintf(nm, sizeof nm, "/ch%d_resynth.wav", ch);
        write_wav(outdir + nm, chres[ch].cmp.aligned_syn, wav_rate);
    }

    std::fprintf(stdout,
        "\n--- R1 · FIDELIDAD DE LA MEZCLA (FM contra FM, PSG muteado) ---\n"
        "  tasa del core    : %.1f Hz (medida)   ymfm: %.2f Hz\n"
        "  desfase corregido: %ld muestras (%.2f ms)   correlación ahí: %.4f\n"
        "  muestras compar. : %zu (%.2f s)   nivel core: %.1f dBFS rms\n"
        "  ganancia óptima  : x%.4f\n"
        "  RESIDUO DEL NULO : %.1f dB\n"
        "  correlación de envolvente: %.4f   (1,0 = mismas notas en el mismo lugar)\n"
        "  brillo (ZCR)     : core %.4f  vs  resynth %.4f\n",
        core_rate, fm_rate, mix.lag, 1000.0 * double(mix.lag) / core_rate,
        mix.lag_corr, mix.n, double(mix.n) / 2.0 / core_rate, mix.core_dbfs,
        mix.gain, mix.null_db, mix.env_corr, mix.zcr_core, mix.zcr_syn);

    std::fprintf(stdout,
        "\n--- R1b · FIDELIDAD POR CANAL (la feature: el canal aislado) ---\n"
        "  canal   nivel core   nivel resynth    nulo    corr.muestra   corr.envolvente\n");
    for (int ch = 0; ch < kFmChannels; ++ch) {
        if (chres[ch].core_lvl < -70.0 && chres[ch].syn_lvl < -70.0) {
            std::fprintf(stdout, "  FM %d    (sin actividad en ninguno de los dos)\n", ch);
            continue;
        }
        std::fprintf(stdout, "  FM %d   %7.1f dBFS   %7.1f dBFS   %6.1f dB      %.4f          %.4f\n",
                     ch, chres[ch].core_lvl, chres[ch].syn_lvl, chres[ch].cmp.null_db,
                     chres[ch].cmp.lag_corr, chres[ch].cmp.env_corr);
    }

    if (psg_core_lvl > -80.0) {
        write_wav(outdir + "/psg_core.wav",    psg_cmp.aligned_core, wav_rate);
        write_wav(outdir + "/psg_resynth.wav", psg_cmp.aligned_syn,  wav_rate);
    }
    std::fprintf(stdout,
        "\n--- R1c · PSG PROPIO (SN76489, mezcla; FM muteado en el core) ---\n"
        "  tasa interna: %.1f Hz   nivel core: %.1f dBFS\n"
        "  MEZCLA  nulo: %.1f dB   corr.muestra: %.4f   corr.envolvente: %.4f%s\n",
        psg_rate, psg_core_lvl, psg_cmp.null_db, psg_cmp.lag_corr, psg_cmp.env_corr,
        psg_core_lvl <= -80.0 ? "   ← el juego no usa el PSG en este pasaje" : "");
    if (psg_core_lvl > -80.0) {
        static const char* kPsgName[4] = { "tono 1", "tono 2", "tono 3", "ruido " };
        std::fprintf(stdout,
            "  canal      nivel core   nivel resynth    nulo    corr.muestra   corr.envolvente\n");
        for (int c = 0; c < kPsgCh; ++c) {
            if (psgres[c].core_lvl < -70.0 && psgres[c].syn_lvl < -70.0) {
                std::fprintf(stdout, "  %s   (sin actividad)\n", kPsgName[c]);
                continue;
            }
            std::fprintf(stdout,
                "  %s   %7.1f dBFS   %7.1f dBFS   %6.1f dB      %.4f          %.4f\n",
                kPsgName[c], psgres[c].core_lvl, psgres[c].syn_lvl,
                psgres[c].cmp.null_db, psgres[c].cmp.lag_corr, psgres[c].cmp.env_corr);
        }
    }

    std::fprintf(stdout,
        "\n--- R2 · JITTER DEL CYCLE ---\n"
        "  frames comparados     : %d   (secuencia distinta en %d)\n"
        "  escrituras comparadas : %llu   con cycle distinto: %llu (%.1f%%)\n"
        "  |delta| máximo        : %ld M-cycles = %.2f muestras de FM\n"
        "  frames con offset CONSTANTE: %d de %d\n",
        frames_compared, frames_seq_differs,
        (unsigned long long)cmp_writes, (unsigned long long)differing,
        cmp_writes ? 100.0 * double(differing) / double(cmp_writes) : 0.0,
        max_delta, double(max_delta) / double(kMCyclesPerFmSample),
        frames_const_offset, frames_compared);

    std::fprintf(stdout,
        "\n--- R3 · COSTO ---\n"
        "  re-síntesis (mezcla + 6 canales) de %zu frames: %.1f ms → %.3f ms/frame\n"
        "  presupuesto de frame: 16,7 ms\n",
        log.size(), synth_ms, synth_ms / double(log.size()));

    std::fprintf(stdout,
        "\n--- R4 · DAC Y SATURACIÓN DEL LOG ---\n"
        "  escrituras: FM=%llu  PSG=%llu  de las cuales DAC (reg 0x2A)=%llu\n"
        "  máximo por frame: %u  (cap del fork: 8192)   frames saturados: %d %s\n",
        (unsigned long long)fm_writes, (unsigned long long)psg_writes,
        (unsigned long long)dac_writes, max_per_frame, saturated_frames,
        saturated_frames ? "← EL LOG PIERDE ESCRITURAS (sound.c:58 trunca en silencio)" : "");

    std::fprintf(stdout,
        "\n--- EL VEREDICTO ES EL OÍDO ---\n"
        "  WAVs en %s (core/resynth/null + ch<N>_core/ch<N>_resynth)\n"
        "  El residuo del nulo INCLUYE el error del resampler (%.0f → %.0f Hz) y\n"
        "  la brecha entre dos emulaciones distintas del YM2612, que no cancelan\n"
        "  nunca a nivel de muestra. Escuchá: si resynth.wav pasa por el original,\n"
        "  sirve; el nulo sólo dice CUÁNTO se parecen las formas de onda.\n",
        outdir.c_str(), fm_rate, core_rate);

    return 0;
}
