// ---------------------------------------------------------------------------
// audio_seek_probe — la sonda de #490: ¿el audio ORIGINAL cambia de timbre
// cuando se llega al mismo frame por seek FRÍO en vez de reproduciendo?
//
// EL SÍNTOMA. «El audio original suena fino/agudo al reproducir tras un seek
// frío». Fino/agudo es una afirmación sobre el CONTENIDO ESPECTRAL, no sobre el
// nivel: un audio puede tener el mismo RMS y sonar completamente distinto. Por
// eso acá se miden las dos cosas, y la que decide es la segunda.
//
// CÓMO SE MIDE SIN OÍDO. El mismo tramo [f0, f0+n) se reproduce dos veces:
//
//   CALIENTE  invalidate → seek(f0-W) … seek(f0+n)   el chip llega al tramo
//             habiendo emulado los W frames previos: su estado (envolventes,
//             fases, LFO) es el que el juego produjo de verdad.
//   FRÍO      invalidate → seek(f0) … seek(f0+n)     se salta directo: el
//             cebado reconstruye ESCRITURAS DE REGISTRO recorriendo la toma,
//             pero no emula audio — y esa es exactamente la hipótesis de #490.
//
// Las dos pasadas vuelcan el PCM del emulador por el tee del player
// (AYTHER_AUDIO_DUMP) y se comparan sobre la MISMA ventana: los últimos n
// frames de cada WAV, que en las dos corresponden a [f0, f0+n).
//
// LAS DOS MEDIDAS.
//   · RMS  — nivel. Si diverge mucho, lo que cambió es el volumen (otro bug).
//   · ZCR  — cruces por cero por muestra. Es el proxy barato del centroide
//            espectral: sube con el contenido agudo y NO necesita FFT. Un tono
//            de 440 Hz a 44,1 kHz cruza ~0,02 veces por muestra; uno de 3 kHz,
//            ~0,14. «Fino/agudo» ES un ZCR más alto.
//
// El veredicto es una RAZÓN, no un absoluto: zcr_frio / zcr_caliente. Igual a 1
// significa que el seek frío entrega el mismo timbre. El umbral por defecto
// (1,15 = 15 % más agudo) sale de que un cambio audible de brillo mueve el
// centroide bastante más que eso; se puede ajustar por env.
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target audio_seek_probe
//   Args:  <recording.ayr> <f0> <n_frames> [warmup_frames]
//   Env:   AYTHER_PROBE_ROM  (el core sale de tests/test_config.toml)
//          AYTHER_SEEK_TOL      (desvio tolerado, default 0.15 = 15 %)
//
// NO reproduce por device: mide el PCM que el emulador entrega, que es lo que
// #490 cuestiona. La entrega (starving, backlog) es otro problema y tiene su
// propia sonda.
// ---------------------------------------------------------------------------
#include "ayther_session.h"
#include "ayther_recording.h"
#include "ayther_env.h"

#include <fstream>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {


// -- Config local: el core sale de tests/test_config.toml (como los demas
//    probes; el archivo es gitignored y apunta al fork compilado) -------------
std::string toml_quoted(const std::string& line) {
    const size_t a = line.find('"');
    const size_t b = line.rfind('"');
    return (a == std::string::npos || b <= a) ? std::string()
                                              : line.substr(a + 1, b - a - 1);
}

std::string resolve(const std::string& p, const std::string& root) {
    if (p.empty()) return p;
    const bool absolute = p.size() > 1 && (p[1] == ':' || p[0] == '/');
    return absolute ? p : root + "/" + p;
}

std::string core_from_config() {
    const std::string root = AYTHER_SOURCE_DIR;
    std::ifstream cfg(root + "/tests/test_config.toml");
    std::string core, line;
    while (std::getline(cfg, line))
        if (core.empty() && line.find("core") != std::string::npos &&
            line.find('=') != std::string::npos)
            core = toml_quoted(line);
    return resolve(core, root);
}

void env_set(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

// -- WAV S16 estéreo: lector mínimo (el mismo formato que deja el tee) --------
std::vector<int16_t> read_wav_s16(const std::string& path) {
    std::vector<int16_t> pcm;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return pcm;
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 44) { std::fclose(f); return pcm; }
    // Cabecera RIFF de 44 bytes: el tee la escribe canónica, así que se saltea
    // sin parsear chunks. Si algún día deja de serlo, esto lo grita (el RMS da 0).
    std::fseek(f, 44, SEEK_SET);
    pcm.resize(static_cast<size_t>(sz - 44) / 2);
    if (std::fread(pcm.data(), 2, pcm.size(), f) != pcm.size()) pcm.clear();
    std::fclose(f);
    return pcm;
}

double rms_of(const int16_t* p, size_t n) {
    if (!n) return 0.0;
    double acc = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double v = p[i] / 32768.0;
        acc += v * v;
    }
    return std::sqrt(acc / static_cast<double>(n));
}

/// Cruces por cero por muestra, con banda muerta: sin ella, el ruido de
/// cuantización de un silencio cruza en cada muestra y da ZCR ~0,5 — el valor
/// de un siseo. La banda es 1/1000 de fondo de escala.
double zcr_of(const int16_t* p, size_t n) {
    if (n < 2) return 0.0;
    const int16_t dead = 32;
    size_t crossings = 0, counted = 0;
    int prev = 0;
    for (size_t i = 0; i < n; ++i) {
        int s = 0;
        if (p[i] >  dead) s =  1;
        else if (p[i] < -dead) s = -1;
        else continue;             // dentro de la banda muerta: no define signo
        if (prev != 0 && s != prev) ++crossings;
        prev = s;
        ++counted;
    }
    return counted ? static_cast<double>(crossings) / static_cast<double>(counted) : 0.0;
}

struct Meas { double rms = 0.0, zcr = 0.0; size_t samples = 0; };

/// Mide la COLA del WAV: los últimos `frames` frames de juego, que es la
/// ventana [f0, f0+n) en las dos pasadas aunque una traiga calentamiento.
Meas measure_tail(const std::string& wav, uint32_t frames, double fps) {
    Meas m;
    const std::vector<int16_t> pcm = read_wav_s16(wav);
    if (pcm.empty()) return m;
    const size_t per_frame = static_cast<size_t>(44100.0 / (fps > 1.0 ? fps : 60.0)) * 2;
    const size_t want = per_frame * frames;
    const size_t take = want && want < pcm.size() ? want : pcm.size();
    const int16_t* p = pcm.data() + (pcm.size() - take);
    m.rms = rms_of(p, take);
    m.zcr = zcr_of(p, take);
    m.samples = take;
    return m;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
            "uso: audio_seek_probe <recording.ayr> <f0> <n_frames> [warmup]\n");
        return 2;
    }
    const std::string core = core_from_config();
    const char*       rom  = ayther::env_get("AYTHER_PROBE_ROM");
    if (core.empty() || !rom) {
        std::fprintf(stderr, "[FAIL] falta core o AYTHER_PROBE_ROM\n");
        return 2;
    }
    const uint32_t f0     = static_cast<uint32_t>(std::atoi(argv[2]));
    const uint32_t n      = static_cast<uint32_t>(std::atoi(argv[3]));
    const uint32_t warmup = argc > 4 ? static_cast<uint32_t>(std::atoi(argv[4])) : 120;
    if (!n) { std::fprintf(stderr, "[FAIL] n_frames = 0\n"); return 2; }
    if (f0 < warmup) {
        std::fprintf(stderr,
            "[FAIL] f0 (%u) < warmup (%u): no hay tramo previo que emular.\n"
            "       Elegí un f0 mas adentro de la toma.\n", f0, warmup);
        return 2;
    }

    auto rec = ayther::AytherRecording::load(argv[1]);
    if (!rec) { std::fprintf(stderr, "[FAIL] no abre la toma\n"); return 1; }

    // Una pasada = una sesión NUEVA con su propio tee. Compartir sesión entre
    // las dos dejaría al chip con el estado de la anterior, que es justo la
    // variable que se está midiendo.
    auto run = [&](const char* wav, uint32_t from) -> double {
        env_set("AYTHER_AUDIO_DUMP", wav);
        ayther::AytherSession::Config c;
        c.core_path = core; c.rom_path = rom; c.enable_audio = true;
        auto r = ayther::AytherSession::create(c);
        if (!r) { std::fprintf(stderr, "[FAIL] create\n"); return -1.0; }
        std::unique_ptr<ayther::AytherSession>& s = *r;
        // AYTHER_SEEK_NO_ROUTER=1 aisla la causa: si con el router de voces
        // APAGADO el frio deja de perder cuerpo, lo que no se repone es el
        // espejo de voces y no el chip del emulador.
        if (ayther::env_get("AYTHER_SEEK_NO_ROUTER")) s->set_voice_router(false);
        s->replay_invalidate();
        for (uint32_t f = from; f < f0 + n; ++f)
            if (!s->replay_seek(*rec, f)) {
                std::fprintf(stderr, "[FAIL] seek f%u\n", f);
                return -1.0;
            }
        const double fps = s->timing_fps();
        return fps > 1.0 ? fps : 60.0;
    };

    std::printf("=== audio_seek_probe (#490) ===\n");
    std::printf("toma: %s · ventana f%u..f%u · warmup %u\n\n",
                argv[1], f0, f0 + n, warmup);

    // Los dos WAV van a build/, como el resto de las sondas: escritos en el cwd
    // quedaban sueltos en la raíz del repo después de cada corrida.
    const std::string out = std::string(AYTHER_SOURCE_DIR) + "/build/";
    const std::string w_hot  = out + "audio_seek_hot.wav";
    const std::string w_cold = out + "audio_seek_cold.wav";

    std::printf("-- CALIENTE: emula desde f%u --\n", f0 - warmup);
    const double fps_hot = run(w_hot.c_str(), f0 - warmup);
    if (fps_hot < 0.0) return 1;

    std::printf("-- FRIO: salta directo a f%u --\n", f0);
    const double fps_cold = run(w_cold.c_str(), f0);
    if (fps_cold < 0.0) return 1;

    const Meas hot  = measure_tail(w_hot,  n, fps_hot);
    const Meas cold = measure_tail(w_cold, n, fps_cold);
    if (!hot.samples || !cold.samples) {
        std::fprintf(stderr,
            "[FAIL] el tee no dejo PCM (¿AYTHER_AUDIO_DUMP soportado?)\n");
        return 1;
    }

    const double zcr_ratio = hot.zcr > 0.0 ? cold.zcr / hot.zcr : 0.0;
    const double rms_ratio = hot.rms > 0.0 ? cold.rms / hot.rms : 0.0;

    std::printf("\n            RMS       ZCR (agudeza)   muestras\n");
    std::printf("caliente  %.5f     %.5f        %zu\n", hot.rms,  hot.zcr,  hot.samples);
    std::printf("frio      %.5f     %.5f        %zu\n", cold.rms, cold.zcr, cold.samples);
    std::printf("razon     %.3f       %.3f\n", rms_ratio, zcr_ratio);

    const char* env_max = ayther::env_get("AYTHER_SEEK_TOL");
    const double tol = env_max ? std::atof(env_max) : 0.15;

    if (hot.rms < 1e-4 && cold.rms < 1e-4) {
        std::printf("\n[SKIP] las dos pasadas son SILENCIO: la ventana no tiene"
                    " audio y la medida no dice nada. Elegi otro tramo.\n");
        return 2;   // no-vacuidad: un silencio no puede pasar como igual
    }

    // El veredicto mira las DOS DIRECCIONES, y esto no es un detalle: la
    // primera version de esta sonda solo fallaba si el frio salia MAS agudo
    // (zcr > 1.15), y dio [ok] sobre una ventana donde el frio tenia el 25 %
    // de la energia del caliente. "Fino" no era "mas agudo": era DELGADO —
    // voces que no arrancan. Un oraculo que mira un solo lado no mide:
    // tranquiliza.
    const double rms_dev = std::fabs(1.0 - rms_ratio);
    const double zcr_dev = std::fabs(1.0 - zcr_ratio);
    if (rms_dev > tol || zcr_dev > tol) {
        std::printf("\n[FAIL] #490 REPRODUCIDO en esta ventana:\n");
        if (rms_dev > tol)
            std::printf("       nivel   %+.1f%% (el frio %s cuerpo)\n",
                        (rms_ratio - 1.0) * 100.0,
                        rms_ratio < 1.0 ? "PIERDE" : "gana");
        if (zcr_dev > tol)
            std::printf("       timbre  %+.1f%% de agudeza (ZCR)\n",
                        (zcr_ratio - 1.0) * 100.0);
        std::printf("       tolerancia: %.0f%% (AYTHER_SEEK_TOL)\n", tol * 100.0);
        return 1;
    }
    std::printf("\n[ok] el seek frio entrega el mismo audio (nivel y timbre"
                " dentro del %.0f%%)\n", tol * 100.0);
    return 0;
}
