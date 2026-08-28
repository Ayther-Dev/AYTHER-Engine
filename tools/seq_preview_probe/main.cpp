// ---------------------------------------------------------------------------
// seq_preview_probe (2026-08-06) — ¿por qué una Secuencia no se escucha?
//
// Reporte: la Secuencia «Opening» (21 eventos) no suena ni en la reproducción
// general ni en el preview; otras del mismo proyecto sí. Lo que tienen de
// distinto sus eventos es DÓNDE caen: los frames 0-35 de la toma, o sea el
// comienzo mismo de la grabación.
//
// El probe mide la ENERGÍA del PCM que produce el motor para un span, por los
// mismos caminos que usa Mezclar, y compara spans:
//
//   · capture_events_pcm  — aislado POR EVENTO (member_sigs): es lo que hace el
//     ▶ de la Secuencia cuando no tiene HD asignado.
//   · capture_channel_pcm — aislado POR CANAL (solo_mask): el fallback.
//   · sin aislamiento     — la mezcla completa del span (¿hay audio ahí?).
//
// Con eso se separa «el span no tiene audio» (problema de la grabación / del
// arranque del chip) de «el aislamiento lo deja mudo» (problema nuestro).
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target seq_preview_probe
//   Args:  <rec.arp> <start> <end> [sig ...]   (sigs en hex, 0x...)
//   Env:   AYTHER_PROBE_ROM
// ---------------------------------------------------------------------------
#include "ayther_env.h"
#include "ayther_session.h"
#include "ayther_recording.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#ifndef AYTHER_SOURCE_DIR
#define AYTHER_SOURCE_DIR "."
#endif

static std::string toml_quoted(const std::string& l) {
    const auto a = l.find('"'), b = l.rfind('"');
    return (a == std::string::npos || b <= a) ? std::string() : l.substr(a + 1, b - a - 1);
}
static std::string resolve(const std::string& p, const std::string& base) {
    if (p.empty() || (p.size() > 1 && p[1] == ':') || p[0] == '/' || p[0] == '\\') return p;
    return base + "/" + p;
}

// RMS y pico del PCM S16 estéreo — «suena» no es booleano: un span puede tener
// muestras y ser inaudible, así que se informan los dos.
static void energy(const std::vector<int16_t>& pcm, double* rms, int* peak) {
    double acc = 0.0; int pk = 0;
    for (int16_t s : pcm) {
        const double v = (double)s;
        acc += v * v;
        pk = (std::max)(pk, std::abs((int)s));
    }
    *rms  = pcm.empty() ? 0.0 : std::sqrt(acc / (double)pcm.size());
    *peak = pk;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
                     "uso: seq_preview_probe <rec.arp> <start> <end> [sig_hex ...]\n");
        return 2;
    }
    const std::string rec_path = argv[1];
    const uint32_t    start    = (uint32_t)std::strtoul(argv[2], nullptr, 10);
    const uint32_t    end      = (uint32_t)std::strtoul(argv[3], nullptr, 10);
    std::vector<uint64_t> sigs;
    for (int i = 4; i < argc; ++i)
        sigs.push_back(std::strtoull(argv[i], nullptr, 16));

    const std::string root = AYTHER_SOURCE_DIR;
    std::string core, rom, line;
    {
        std::ifstream cfg(root + "/tests/test_config.toml");
        while (std::getline(cfg, line))
            if (line.find("core") != std::string::npos &&
                line.find('=') != std::string::npos && core.empty())
                core = toml_quoted(line);
    }
    core = resolve(core, root);
    if (const char* er = ayther::env_get("AYTHER_PROBE_ROM")) rom = er;
    if (rom.empty()) { std::fprintf(stderr, "[FAIL] falta AYTHER_PROBE_ROM\n"); return 2; }

    ayther::AytherSession::Config c;
    c.core_path = core; c.rom_path = rom; c.enable_audio = true;
    auto r = ayther::AytherSession::create(c);
    if (!r) { std::fprintf(stderr, "[FAIL] create: %s\n", r.error.message.c_str()); return 1; }
    std::unique_ptr<ayther::AytherSession>& s = *r;
    auto rec = ayther::AytherRecording::load(rec_path);
    if (!rec) { std::fprintf(stderr, "[FAIL] no se pudo cargar %s\n", rec_path.c_str()); return 1; }

    std::printf("toma: %u frames · trim [%u, %u)\n",
                rec->frame_count(), rec->trim_in, rec->trim_out);
    std::printf("span pedido: [%u, %u]  (%u frames) · %zu firmas\n\n",
                start, end, end >= start ? end - start + 1u : 0u, sigs.size());

    // El análisis de eventos es lo que puebla audio_events (y con él el
    // aislamiento por firma); sin esto el camino «por evento» no existe.
    s->analyze_audio_events(*rec);
    std::printf("eventos analizados en la toma: %u\n", s->audio_event_count());

    const uint32_t win = end >= start ? (end - start + 1u) : 1u;

    // Canales que tocan las firmas pedidas → solo_mask del fallback por canal.
    uint16_t solo = 0;
    {
        const AytherAudioEvent* evs = s->audio_events();
        const uint32_t n = s->audio_event_count();
        for (uint32_t i = 0; i < n; ++i)
            for (uint64_t sg : sigs)
                if (evs[i].signature == sg)
                    solo |= evs[i].chip == 0 ? (uint16_t)(1u << evs[i].channel)
                                             : (uint16_t)(1u << (6 + evs[i].channel));
    }
    std::printf("solo_mask derivada de las firmas: 0x%04X\n\n", solo);

    std::vector<int16_t> pcm;
    double rms; int peak;

    if (!sigs.empty()) {
        const size_t fr = s->capture_events_pcm(*rec, start, win, sigs, pcm);
        energy(pcm, &rms, &peak);
        std::printf("POR EVENTO  (member_sigs): %6zu cuadros · rms %8.1f · pico %6d\n",
                    fr, rms, peak);
    }
    if (solo) {
        const size_t fr = s->capture_channel_pcm(*rec, start, win, solo, pcm);
        energy(pcm, &rms, &peak);
        std::printf("POR CANAL   (solo_mask)  : %6zu cuadros · rms %8.1f · pico %6d\n",
                    fr, rms, peak);
    }
    {   // Mezcla completa del mismo span: ¿hay AUDIO ahí, sin aislar nada?
        const size_t fr = s->capture_channel_pcm(*rec, start, win, 0x03FF, pcm);
        energy(pcm, &rms, &peak);
        std::printf("MEZCLA      (todo audible): %6zu cuadros · rms %8.1f · pico %6d\n",
                    fr, rms, peak);

        // Desglose por FRAME dentro del span. Es lo que separa las dos causas
        // posibles de un span mudo: si el silencio está sólo al principio y el
        // audio entra al mismo frame ABSOLUTO sin importar dónde empiece la
        // ventana, es el CONTENIDO; si depende del largo de la ventana, es el
        // arranque de la re-simulación (warmup) y entonces es problema nuestro.
        if (fr > 0 && win > 1) {
            const size_t per = pcm.size() / win;   // muestras por frame
            std::printf("\n  desglose por frame (rms):\n   ");
            uint32_t first_audible = UINT32_MAX;
            for (uint32_t f = 0; f < win; ++f) {
                const size_t a = (size_t)f * per;
                if (a + per > pcm.size()) break;
                std::vector<int16_t> blk(pcm.begin() + a, pcm.begin() + a + per);
                double brms; int bpk;
                energy(blk, &brms, &bpk);
                if (brms > 1.0 && first_audible == UINT32_MAX)
                    first_audible = start + f;
                if (win <= 64 || (f % (win / 48 + 1)) == 0)
                    std::printf("%s", brms > 1.0 ? "#" : ".");
            }
            std::printf("\n  primer frame con audio: ");
            if (first_audible == UINT32_MAX) std::printf("NINGUNO\n");
            else                             std::printf("%u\n", first_audible);
        }
    }
    // ---- El MISMO span por el camino del CORE ------------------------------
    // capture_channel_pcm y capture_events_pcm intentan PRIMERO el espejo
    // (preview_render_base) y sólo caen al core si el espejo no puede — así que
    // todo lo de arriba mide el ESPEJO. export_audio_event_wav con solo=0 y sin
    // firmas es la mezcla completa del CORE: si acá hay audio y arriba no, el
    // mudo lo pone el espejo (y es nuestro), no la grabación.
    {
        const std::string wav = root + "/build/seq_preview_probe.wav";
        const bool ok = s->export_audio_event_wav(*rec, start, end, wav.c_str(),
                                                  /*tail=*/0, /*solo_mask=*/0,
                                                  /*member_sigs=*/nullptr);
        if (!ok) {
            std::printf("\nCORE (export wav): FALLÓ\n");
        } else {
            std::ifstream f(wav, std::ios::binary);
            f.seekg(44);   // header canónico de 44 bytes
            std::vector<int16_t> w((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>()), pcmw;
            // (el vector de arriba se llenó como bytes: releer bien)
            f.clear(); f.seekg(0, std::ios::end);
            const size_t bytes = (size_t)f.tellg() - 44;
            f.seekg(44);
            pcmw.resize(bytes / 2);
            f.read(reinterpret_cast<char*>(pcmw.data()), (std::streamsize)bytes);
            double wr; int wp;
            energy(pcmw, &wr, &wp);
            std::printf("\nCORE (mezcla, wav): %6zu cuadros · rms %8.1f · pico %6d\n",
                        pcmw.size() / 2, wr, wp);
        }
    }
    return 0;
}
