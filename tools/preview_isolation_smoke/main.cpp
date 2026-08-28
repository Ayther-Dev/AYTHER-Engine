// ---------------------------------------------------------------------------
// preview_isolation_smoke — los previews A/B aislados salen del espejo (#338).
//
// El discriminador es el mismo residuo de #282: con la captura por máscara,
// aislar un sonido dejaba pasar el ring y los pokes del resto (~-40 dBFS en
// los frames «callados»). Con el aislamiento por espejo, lo que está fuera de
// las ventanas del sonido es CERO digital — no hay voz que emita.
//
// Asertos:
//   1. capture_events_pcm de una firma: suena (hay señal en sus ventanas) y
//      los frames FUERA de toda ventana+cola son ~silencio digital.
//   2. capture_channel_pcm de un canal: suena.
//   3. Con el router APAGADO, el camino legacy sigue vivo (fallback).
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target preview_isolation_smoke
//   Args:  <core.dll> <rom> <recording.arp>
// ---------------------------------------------------------------------------
#include "ayther_session.h"
#include "ayther_recording.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using ayther::AytherSession;

namespace {

double to_dbfs(int32_t peak) {
    return peak > 0 ? 20.0 * std::log10(double(peak) / 32768.0) : -999.0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
            "preview_isolation_smoke — aislamiento por espejo (#338)\n"
            "uso: %s <core.dll> <rom> <recording.arp>\n", argv[0]);
        return 2;
    }
    AytherSession::Config cfg;
    cfg.core_path    = argv[1];
    cfg.rom_path     = argv[2];
    cfg.enable_audio = false;
    auto r = AytherSession::create(cfg);
    if (!r) { std::fprintf(stderr, "[FAIL] create\n"); return 1; }
    std::unique_ptr<AytherSession>& s = *r;

    auto rec_opt = ayther::AytherRecording::load(argv[3]);
    if (!rec_opt) { std::fprintf(stderr, "[FAIL] rec\n"); return 1; }
    const ayther::AytherRecording& rec = *rec_opt;
    const uint32_t nf = rec.frame_count();

    const uint32_t nev = s->analyze_audio_events(rec);
    const AytherAudioEvent* evs = s->audio_events();
    if (nev < 4) { std::fprintf(stderr, "[FAIL] pocos eventos\n"); return 1; }

    // La firma FM que más se repite: ventanas de sobra para clasificar frames.
    uint64_t member_sig = 0; int best = 0; uint16_t member_bit = 0;
    for (uint32_t i = 0; i < nev; ++i) {
        if (evs[i].chip != 0 || evs[i].pitch == 255) continue;
        int reps = 0;
        for (uint32_t j = 0; j < nev; ++j)
            if (evs[j].signature == evs[i].signature) ++reps;
        if (reps > best) { best = reps; member_sig = evs[i].signature;
                           member_bit = uint16_t(1u << evs[i].channel); }
    }
    if (!member_sig) { std::fprintf(stderr, "[FAIL] sin firma FM\n"); return 1; }
    std::printf("firma %016llx (%d ocurrencias), canal bit 0x%03x, %u frames\n",
                (unsigned long long)member_sig, best, member_bit, nf);

    int fails = 0;
    auto check = [&](bool ok, const char* what, double val) {
        std::printf("[%s] %s (%.1f dBFS)\n", ok ? "OK " : "FAIL", what, val);
        if (!ok) ++fails;
    };

    // -- 1. Aislar por EVENTO: señal adentro, cero digital afuera -------------
    std::vector<uint64_t> sigs = { member_sig };
    std::vector<int16_t>  pcm;
    const size_t frames = s->capture_events_pcm(rec, 0, nf, sigs, pcm);
    if (!frames) { std::fprintf(stderr, "[FAIL] capture_events_pcm\n"); return 1; }

    // Clasificación por frame con acumulador FRACCIONARIO (la lección de
    // mute_silence_probe: la división entera corre la ventana ~5 frames).
    constexpr uint32_t kTail = 15, kSlack = 2;
    std::vector<uint8_t> inside(nf, 0);
    for (uint32_t i = 0; i < nev; ++i) {
        if (evs[i].signature != member_sig) continue;
        const uint32_t e1 = (std::min)(evs[i].end_frame + kTail + kSlack, nf - 1);
        for (uint32_t f = evs[i].start_frame > kSlack ? evs[i].start_frame - kSlack : 0;
             f <= e1; ++f) inside[f] = 1;
    }
    const double per_frame = double(frames) / double(nf);
    int32_t peak_in = 0, peak_out = 0;
    for (uint32_t f = 0; f < nf; ++f) {
        size_t a = size_t(per_frame * f + 0.5) * 2;
        size_t b = (std::min)(size_t(per_frame * (f + 1) + 0.5) * 2, pcm.size());
        int32_t p = 0;
        for (size_t i = a; i < b; ++i) {
            const int32_t v = pcm[i] < 0 ? -int32_t(pcm[i]) : int32_t(pcm[i]);
            if (v > p) p = v;
        }
        if (inside[f]) { if (p > peak_in)  peak_in  = p; }
        else           { if (p > peak_out) peak_out = p; }
    }
    check(to_dbfs(peak_in) > -30.0, "por evento: el sonido esta", to_dbfs(peak_in));
    check(to_dbfs(peak_out) < -60.0, "por evento: fuera de ventana+cola, silencio",
          to_dbfs(peak_out));

    // -- 2. Aislar por CANAL: suena ------------------------------------------
    const size_t fr2 = s->capture_channel_pcm(rec, 0, nf, member_bit, pcm);
    int32_t p2 = 0;
    for (int16_t v : pcm) { const int32_t a = v < 0 ? -v : v; if (a > p2) p2 = a; }
    check(fr2 > 0 && to_dbfs(p2) > -30.0, "por canal: el canal esta", to_dbfs(p2));

    // -- 3. Router apagado → el fallback legacy sigue vivo --------------------
    s->set_voice_router(false);
    const size_t fr3 = s->capture_events_pcm(rec, 0, (std::min)(nf, 600u), sigs, pcm);
    std::printf("[%s] fallback legacy con router apagado (%zu frames)\n",
                fr3 > 0 ? "OK " : "FAIL", fr3);
    if (!fr3) ++fails;

    std::printf("\n%s\n", fails == 0
        ? "preview_isolation_smoke: PASS — aislar es cero digital fuera del pedido"
        : "preview_isolation_smoke: FAIL");
    return fails == 0 ? 0 : 5;
}
