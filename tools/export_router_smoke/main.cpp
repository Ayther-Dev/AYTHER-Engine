// ---------------------------------------------------------------------------
// export_router_smoke — el export rinde por el router (#331). Tres asertos:
//
//   1. SILENCIO REAL — con TODOS los instrumentos silenciados, el WAV del
//      mixdown hd debe quedar MUY por debajo del residuo del camino viejo
//      (mute_silence_probe midió -33,7 dBFS de pico con la máscara; acá una
//      voz silenciada no EMITE, así que el piso es otro orden).
//   2. LA MÚSICA ESTÁ — sin mutes, el mixdown hd tiene señal de música
//      (la composición del router suena, no exportamos silencio por error).
//   3. EL ORIGINAL ES EL CHIP — hd=false sigue saliendo del emulador (no
//      cambia con #331) y tiene señal.
//
// Además reporta el residuo del CAMINO VIEJO (router apagado) como referencia
// — es el número contra el que se compara el aserto 1.
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target export_router_smoke
//   Args:  <core.dll> <rom> <recording.arp> [dir_salida]
// ---------------------------------------------------------------------------
#include "ayther_session.h"
#include "ayther_recording.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

using ayther::AytherSession;

namespace {

// WAV S16 estéreo mínimo (mismo lector que mute_silence_probe).
bool read_wav_s16(const std::string& path, std::vector<int16_t>& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> all;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 44) { std::fclose(f); return false; }
    all.resize(static_cast<size_t>(n));
    const bool ok = std::fread(all.data(), 1, all.size(), f) == all.size();
    std::fclose(f);
    if (!ok || std::memcmp(all.data(), "RIFF", 4) != 0) return false;
    size_t p = 12;
    while (p + 8 <= all.size()) {
        char id[5] = {0};
        std::memcpy(id, all.data() + p, 4);
        uint32_t sz = 0;
        std::memcpy(&sz, all.data() + p + 4, 4);
        const size_t body = p + 8;
        if (std::memcmp(id, "data", 4) == 0) {
            const size_t avail = std::min<size_t>(sz, all.size() - body);
            out.resize(avail / 2);
            std::memcpy(out.data(), all.data() + body, out.size() * 2);
            return true;
        }
        p = body + sz + (sz & 1);
    }
    return false;
}

double peak_dbfs(const std::vector<int16_t>& pcm) {
    int32_t peak = 0;
    for (int16_t v : pcm) {
        const int32_t a = v < 0 ? -int32_t(v) : int32_t(v);
        if (a > peak) peak = a;
    }
    return peak > 0 ? 20.0 * std::log10(double(peak) / 32768.0) : -999.0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
            "export_router_smoke — el export rinde por el router (#331)\n"
            "uso: %s <core.dll> <rom> <recording.arp> [dir_salida]\n", argv[0]);
        return 2;
    }
    const std::string out_dir = argc > 4 ? argv[4] : ".";

    AytherSession::Config cfg;
    cfg.core_path    = argv[1];
    cfg.rom_path     = argv[2];
    cfg.enable_audio = false;    // el export no necesita device — ése es el punto
    auto r = AytherSession::create(cfg);
    if (!r) { std::fprintf(stderr, "[FAIL] create\n"); return 1; }
    std::unique_ptr<AytherSession>& s = *r;

    auto rec_opt = ayther::AytherRecording::load(argv[3]);
    if (!rec_opt) { std::fprintf(stderr, "[FAIL] rec\n"); return 1; }
    const ayther::AytherRecording& rec = *rec_opt;

    const uint32_t nev = s->analyze_audio_events(rec);
    if (!nev) { std::fprintf(stderr, "[FAIL] sin eventos\n"); return 1; }
    const AytherAudioEvent* evs = s->audio_events();
    std::unordered_set<uint64_t> uniq;
    for (uint32_t i = 0; i < nev; ++i)
        if (evs[i].instrument) uniq.insert(evs[i].instrument);
    std::vector<uint64_t> insts(uniq.begin(), uniq.end());
    std::printf("toma: %u frames, %u eventos, %zu timbres\n",
                rec.frame_count(), nev, insts.size());

    const uint32_t end = rec.frame_count();
    int fails = 0;
    auto check = [&](bool ok, const char* what, double val) {
        std::printf("[%s] %s (%.1f dBFS)\n", ok ? "OK " : "FAIL", what, val);
        if (!ok) ++fails;
    };

    // -- 1. Todo silenciado → el mixdown hd es silencio de verdad -------------
    s->set_audio_instrument_mute(insts.data(), uint32_t(insts.size()));
    const std::string w_muted = out_dir + "/export_muted.wav";
    if (!s->export_mixdown_wav(rec, 0, end, w_muted.c_str(), /*hd=*/true)) {
        std::fprintf(stderr, "[FAIL] export hd (muted)\n"); return 1;
    }
    std::vector<int16_t> pcm;
    if (!read_wav_s16(w_muted, pcm)) { std::fprintf(stderr, "[FAIL] leer WAV\n"); return 1; }
    const double muted_peak = peak_dbfs(pcm);
    // El umbral es EL DEL PROBLEMA: el camino viejo dejaba -33,7 dBFS de pico
    // (mute_silence_probe). Exigimos 20 dB mejor — silenciar tiene que callar.
    check(muted_peak < -55.0, "todo silenciado: el export calla", muted_peak);

    // -- 2. Sin mutes → la composición suena ----------------------------------
    s->set_audio_instrument_mute(nullptr, 0);
    const std::string w_full = out_dir + "/export_full.wav";
    if (!s->export_mixdown_wav(rec, 0, end, w_full.c_str(), /*hd=*/true)) {
        std::fprintf(stderr, "[FAIL] export hd (full)\n"); return 1;
    }
    if (!read_wav_s16(w_full, pcm)) { std::fprintf(stderr, "[FAIL] leer WAV\n"); return 1; }
    const double full_peak = peak_dbfs(pcm);
    check(full_peak > -20.0, "sin mutes: la musica esta", full_peak);

    // -- 3. hd=false → el original sigue siendo el chip real ------------------
    const std::string w_orig = out_dir + "/export_original.wav";
    if (!s->export_mixdown_wav(rec, 0, end, w_orig.c_str(), /*hd=*/false)) {
        std::fprintf(stderr, "[FAIL] export original\n"); return 1;
    }
    if (!read_wav_s16(w_orig, pcm)) { std::fprintf(stderr, "[FAIL] leer WAV\n"); return 1; }
    const double orig_peak = peak_dbfs(pcm);
    check(orig_peak > -20.0, "hd=false: el original (chip) esta", orig_peak);

    // -- Referencia: el residuo del camino viejo, para el registro ------------
    s->set_voice_router(false);
    s->set_audio_instrument_mute(insts.data(), uint32_t(insts.size()));
    const std::string w_legacy = out_dir + "/export_legacy_muted.wav";
    double legacy_peak = -999.0;
    if (s->export_mixdown_wav(rec, 0, end, w_legacy.c_str(), /*hd=*/true) &&
        read_wav_s16(w_legacy, pcm))
        legacy_peak = peak_dbfs(pcm);
    std::printf("(ref) camino viejo todo silenciado: %.1f dBFS de pico — el residuo de #282\n",
                legacy_peak);

    std::printf("\n%s\n", fails == 0
        ? "export_router_smoke: PASS — la preview y el entregable son el mismo render"
        : "export_router_smoke: FAIL");
    return fails == 0 ? 0 : 5;
}
