// ---------------------------------------------------------------------------
// audio_preview_smoke — prueba de integración de AytherSession::preview_audio.
//
// Carga un core + ROM reales (de tests/test_config.toml), graba una toma con
// audio, y verifica los invariantes de la VISTA PREVIA de audio por hash:
//
//   1. captura: preview_audio(hash conocido) devuelve PCM > 0 (aislado por hash).
//   2. sin efecto: el estado de la sesión queda IDÉNTICO tras el preview
//      (serialize antes == después) → confirma que NO mueve el playhead.
//   3. hash inexistente → captura 0 (no rompe).
//
// La captura no necesita device de audio (enable_audio=false) — el callback de
// audio se dispara igual en run_frame. Headless. Lee la config para no hardcodear
// rutas (ver tests/test_config.example.toml).
//
//   Build:  -DAYTHER_BUILD_SPIKE=ON  →  target audio_preview_smoke
//   Run:    bin/audio_preview_smoke[.exe]   (toma rutas de tests/test_config.toml)
// ---------------------------------------------------------------------------
#include "ayther_session.h"
#include "ayther_recording.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef AYTHER_SOURCE_DIR
#define AYTHER_SOURCE_DIR "."
#endif

// Valor entre comillas de una línea TOML simple (key = "valor").
static std::string quoted(const std::string& line) {
    const auto a = line.find('"');
    const auto b = line.rfind('"');
    if (a == std::string::npos || b <= a) return {};
    return line.substr(a + 1, b - a - 1);
}

// Resuelve una ruta relativa contra la raíz del repo (rutas absolutas tal cual).
static std::string resolve(const std::string& p, const std::string& base) {
    if (p.empty()) return p;
    if (p.size() > 1 && p[1] == ':') return p;          // C:/...
    if (p[0] == '/' || p[0] == '\\')  return p;
    return base + "/" + p;
}

int main() {
    const std::string root = AYTHER_SOURCE_DIR;
    const std::string cfg_path = root + "/tests/test_config.toml";

    std::ifstream cfg(cfg_path);
    if (!cfg) {
        std::fprintf(stderr,
            "[skip] no existe %s — copiá tests/test_config.example.toml y completá las rutas.\n",
            cfg_path.c_str());
        return 0;   // sin config no se puede correr; no es una falla del feature
    }
    std::string core, rom, line;
    bool in_rom = false;
    while (std::getline(cfg, line)) {
        if (line.find("[[rom]]") != std::string::npos) { in_rom = true; continue; }
        if (line.find("core") != std::string::npos && line.find('=') != std::string::npos && core.empty())
            core = quoted(line);
        else if (in_rom && rom.empty() && line.find("path") != std::string::npos)
            rom = quoted(line);
    }
    core = resolve(core, root);
    rom  = resolve(rom,  root);
    if (core.empty() || rom.empty()) {
        std::fprintf(stderr, "[FAIL] config incompleta (core='%s' rom='%s')\n", core.c_str(), rom.c_str());
        return 1;
    }
    std::printf("=== audio_preview_smoke ===\ncore: %s\nrom : %s\n\n", core.c_str(), rom.c_str());

    ayther::AytherSession::Config c;
    c.core_path    = core;
    c.rom_path     = rom;
    c.enable_audio = false;   // headless: la captura no necesita device de audio
    auto r = ayther::AytherSession::create(c);
    if (!r) { std::fprintf(stderr, "[FAIL] create: %s\n", r.error.message.c_str()); return 1; }
    std::unique_ptr<ayther::AytherSession>& s = *r;

    constexpr uint16_t START = 1u << 3;   // RETRO_DEVICE_ID_JOYPAD_START
    for (int i = 0; i < 120; ++i) { s->set_input(0, (i % 32 < 4) ? START : 0); s->step(); }  // warmup

    // Grabar una toma con audio (el .arp resultante trae la historia v7).
    s->record_start();
    constexpr int kFrames = 400;
    for (int i = 0; i < kFrames; ++i) { s->set_input(0, (i % 16 < 2) ? START : 0); s->step(); }
    s->record_stop();
    ayther::AytherRecording take = s->take_recording();
    std::printf("[ok] toma: %u frames, %zu hashes de audio\n",
                take.frame_count(), take.audio_hashes.size());

    int fail = 0;
    auto check = [&](bool ok, const char* msg) {
        std::printf("[%s] %s\n", ok ? "ok" : "FAIL", msg);
        if (!ok) ++fail;
    };

    check(take.audio_offsets.size() == take.frame_count() + 1, "la toma tiene CSR de audio (v7)");
    check(!take.audio_hashes.empty(), "la toma capturó audio");

    if (!take.audio_hashes.empty()) {
        // --- Diagnóstico: ¿el REPLAY reproduce los hashes de audio GRABADOS? ----
        // (de eso dependen el preview Y el mute-por-hash). Buscamos un frame con
        // audio, hacemos replay_seek a ÉL y comparamos sus occurrences con las del
        // .arp.
        uint32_t f = UINT32_MAX;
        for (uint32_t i = 0; i + 1 < take.audio_offsets.size(); ++i)
            if (take.audio_offsets[i + 1] > take.audio_offsets[i]) { f = i; break; }
        std::printf("[..] primer frame con audio = %u\n", f);

        if (f != UINT32_MAX) {
            std::unordered_set<uint64_t> recorded;
            for (uint32_t k = take.audio_offsets[f]; k < take.audio_offsets[f + 1]; ++k)
                recorded.insert(take.audio_hashes[k]);

            const ayther::FrameView* fv = s->replay_seek(take, f);
            const uint64_t h1 = (fv && fv->audio_occ_count) ? fv->audio_occs[0].hash : 0;
            // ¿El replay es AUTO-consistente? (re-seek frío al mismo frame → mismo hash)
            s->replay_seek(take, f + 40 < take.frame_count() ? f + 40 : 0);
            const ayther::FrameView* fv2 = s->replay_seek(take, f);
            const uint64_t h2 = (fv2 && fv2->audio_occ_count) ? fv2->audio_occs[0].hash : 0;
            std::printf("[..] replay self: h1=0x%016llx h2=0x%016llx %s\n",
                        static_cast<unsigned long long>(h1), static_cast<unsigned long long>(h2),
                        (h1 == h2 && h1) ? "(consistente)" : "(INESTABLE)");
            int matches = 0;
            std::printf("[..] frame %u: grabados=%zu  replay_occs=%u\n",
                        f, recorded.size(), fv ? fv->audio_occ_count : 0);
            if (fv) for (uint32_t i = 0; i < fv->audio_occ_count; ++i) {
                if (recorded.count(fv->audio_occs[i].hash)) ++matches;
            }
            // HALLAZGO (informativo, no es falla de este código): el audio NO es
            // byte-reproducible a través del save/load del replay (el video sí). El
            // replay es auto-consistente (h1==h2) pero sus hashes ≠ los grabados →
            // identificar audio por el hash GRABADO no funciona en replay. Afecta
            // tanto al preview aislado como al mute-por-hash en Editar (Fase 3).
            std::printf("[%s] audio replay vs grabado: %d/%u coinciden  %s\n",
                        matches > 0 ? "ok" : "KNOWN", matches, fv ? fv->audio_occ_count : 0,
                        matches > 0 ? "" : "(audio no reproducible cross-seek — ver reporte)");

            // --- ¿El audio del replay es estable POR FRAME (independiente del
            // camino)? secuencial (play, cadena +1) vs cold-seek (scrub). Si difiere,
            // ni los hashes del replay sirven como identidad estable. ----------------
            uint32_t n = take.frame_count() / 3;
            while (n + 1 < take.audio_offsets.size() &&
                   take.audio_offsets[n + 1] == take.audio_offsets[n]) ++n;   // frame con audio
            uint64_t seq_hash = 0;
            for (uint32_t i = 0; i <= n; ++i) {
                const ayther::FrameView* v = s->replay_seek(take, i);         // cadena +1 (play)
                if (i == n && v && v->audio_occ_count) seq_hash = v->audio_occs[0].hash;
            }
            s->replay_seek(take, take.frame_count() - 1);                     // forzar cold
            const ayther::FrameView* vc = s->replay_seek(take, n);            // cold-seek (scrub)
            const uint64_t cold_hash = (vc && vc->audio_occ_count) ? vc->audio_occs[0].hash : 0;
            std::printf("[%s] audio path-stable @frame %u: seq=0x%016llx cold=0x%016llx %s\n",
                        (seq_hash == cold_hash && seq_hash) ? "ok" : "KNOWN", n,
                        static_cast<unsigned long long>(seq_hash),
                        static_cast<unsigned long long>(cold_hash),
                        (seq_hash == cold_hash && seq_hash) ? "(estable)" : "(DEPENDE DEL CAMINO)");
            // ¿La reproducción CONTINUA desde 0 reproduce el hash GRABADO en n?
            std::unordered_set<uint64_t> rec_n;
            for (uint32_t k = take.audio_offsets[n]; k < take.audio_offsets[n + 1]; ++k)
                rec_n.insert(take.audio_hashes[k]);
            const bool seq_matches_rec = rec_n.count(seq_hash) > 0;
            std::printf("[%s] continua-desde-0 vs grabado @%u: %s\n",
                        seq_matches_rec ? "ok" : "KNOWN", n,
                        seq_matches_rec ? "coincide → re-sim continua sirve"
                                        : "no coincide (ni la continua reproduce lo grabado)");

            // Invariantes que ESTE código sí garantiza (hard checks):
            const uint32_t mid = take.frame_count() / 2;
            s->replay_seek(take, mid);
            std::vector<uint8_t> before; const bool ser_ok = static_cast<bool>(s->serialize(before));
            check(ser_ok && !before.empty(), "serialize del estado del playhead");

            // preview con un hash inexistente NO debe capturar ni alterar el estado.
            const size_t bogus = s->preview_audio(take, 0xDEADBEEFCAFEBABEull);
            check(bogus == 0, "hash inexistente → captura 0");

            // Preview EN CONTEXTO: ubica el frame por el .arp (hash grabado) y captura
            // la mezcla de ese momento → debe capturar PCM > 0.
            const size_t cap = s->preview_audio(take, recorded.empty() ? 0 : *recorded.begin());
            std::printf("[..] preview en contexto → %zu frames\n", cap);
            check(cap > 0, "preview en contexto capturó la mezcla del frame del sonido");

            // No mueve el playhead: estado idéntico tras el preview.
            std::vector<uint8_t> after; s->serialize(after);
            check(ser_ok && before == after, "estado IDÉNTICO tras el preview (no movió el playhead)");
        }
    }

    std::printf("\n%s (%d fallos)\n", fail ? "FALLÓ" : "OK", fail);
    return fail ? 1 : 0;
}
