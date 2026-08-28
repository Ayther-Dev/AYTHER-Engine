// ---------------------------------------------------------------------------
// session_smoke — headless runtime validation of AytherSession (R2.3 de-risk).
//
// The R2 facade compiles, but compiling does not prove the per-frame pipeline
// runs. This harness drives the real motor with no GUI:
//
//   create() -> warmup -> serialize(S) -> run N frames (hash each framebuffer)
//             -> unserialize(S) -> run N frames (hash) -> assert sequences equal
//
// It proves three things at once, end-to-end through the facade:
//   1. create()/step() actually run (no crash, sane FrameView).
//   2. the pipeline produces data (framebuffers + occurrences).
//   3. step() stays deterministic across serialize/unserialize (the .arp base).
//
// No audio device is opened (Config::enable_audio = false), so it runs in CI /
// headless. See docs/architecture/ayther-engine.md §5, §6.1.
// ---------------------------------------------------------------------------

#include "ayther_session.h"
#include "ayther_recording.h"   // R7: .arp recording round-trip check

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

static uint64_t fnv1a(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

// Hash this frame's framebuffer; carry the previous hash on a duplicated/empty
// frame (the core skipped a redraw — a legitimate, reproducible state).
static uint64_t hash_fb(const ayther::FrameView& fv, uint64_t prev) {
    if (!fv.fb_pixels || fv.fb_height == 0 || fv.fb_pitch == 0) return prev;
    return fnv1a(static_cast<const uint8_t*>(fv.fb_pixels),
                 static_cast<size_t>(fv.fb_height) * fv.fb_pitch);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
            "usage: %s <core.dll> <rom> [--warmup N] [--frames N]\n", argv[0]);
        return 2;
    }

    ayther::AytherSession::Config cfg;
    cfg.core_path    = argv[1];
    cfg.rom_path     = argv[2];
    cfg.enable_audio = false;            // headless: do not open an audio device

    int warmup = 120, frames = 300;
    for (int i = 3; i + 1 < argc; ++i) {
        if      (!std::strcmp(argv[i], "--warmup")) warmup = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--frames")) frames = std::atoi(argv[i + 1]);
    }

    std::printf("=== AytherSession smoke (R2.3 de-risk) ===\n");
    std::printf("core: %s\nrom : %s\nwarmup=%d frames=%d\n\n",
                cfg.core_path.c_str(), cfg.rom_path.c_str(), warmup, frames);

    auto r = ayther::AytherSession::create(cfg);
    if (!r) {
        std::fprintf(stderr, "[FAIL] create: %s\n", r.error.message.c_str());
        return 1;
    }
    std::unique_ptr<ayther::AytherSession>& sess = *r;
    std::printf("[ok] created. game_id='%s' timing_fps=%.3f has_pack=%d\n",
                sess->game_id(), sess->timing_fps(), static_cast<int>(sess->has_pack()));

    constexpr uint16_t START = 1u << 3;  // RETRO_DEVICE_ID_JOYPAD_START

    // Warmup: tap START periodically to push past the title screen into gameplay.
    uint32_t max_tile = 0, max_sprite = 0, max_audio = 0, last_unique_tiles = 0;
    uint32_t max_chip = 0; uint64_t total_chip = 0;
    uint64_t fb_present = 0;
    for (int i = 0; i < warmup; ++i) {
        sess->set_input(0, (i % 32 < 4) ? START : 0);
        const ayther::FrameView& fv = sess->step();
        if (fv.fb_pixels) ++fb_present;
        if (fv.tile_occ_count   > max_tile)   max_tile   = fv.tile_occ_count;
        if (fv.sprite_occ_count > max_sprite) max_sprite = fv.sprite_occ_count;
        if (fv.audio_occ_count  > max_audio)  max_audio  = fv.audio_occ_count;
        if (fv.chip_write_count > max_chip)   max_chip   = fv.chip_write_count;
        total_chip += fv.chip_write_count;
        last_unique_tiles = fv.unique_tile_count;
    }
    std::printf("[ok] warmup %d frames: fb_present=%llu max_tile_occ=%u "
                "max_sprite_occ=%u max_audio_occ=%u unique_tiles=%u\n",
                warmup, static_cast<unsigned long long>(fb_present),
                max_tile, max_sprite, max_audio, last_unique_tiles);
    std::printf("[%s] chip writes (Audios C-A1): max/frame=%u total=%llu\n",
                (total_chip > 0) ? "ok" : "warn", max_chip,
                static_cast<unsigned long long>(total_chip));

    // Savestate snapshot.
    std::vector<uint8_t> snap;
    if (auto sr = sess->serialize(snap); !sr) {
        std::fprintf(stderr, "[FAIL] serialize: %s\n", sr.error.message.c_str());
        return 1;
    }
    std::printf("[ok] savestate = %zu bytes\n", snap.size());

    // Run A: N frames from the snapshot with zero input (fully reproducible).
    std::vector<uint64_t> seqA(static_cast<size_t>(frames));
    uint64_t h = 0;
    for (int i = 0; i < frames; ++i) { sess->set_input(0, 0); h = hash_fb(sess->step(), h); seqA[i] = h; }

    // Reload the same snapshot and run B identically.
    if (auto ur = sess->unserialize(snap); !ur) {
        std::fprintf(stderr, "[FAIL] unserialize: %s\n", ur.error.message.c_str());
        return 1;
    }
    std::vector<uint64_t> seqB(static_cast<size_t>(frames));
    h = 0;
    for (int i = 0; i < frames; ++i) { sess->set_input(0, 0); h = hash_fb(sess->step(), h); seqB[i] = h; }

    // Compare the two runs frame-by-frame.
    int first_div = -1;
    for (int i = 0; i < frames; ++i) if (seqA[i] != seqB[i]) { first_div = i; break; }

    // -----------------------------------------------------------------------
    // R7: recording round-trip determinism.
    // Record `frames` frames from the snapshot with a fixed input pattern, then
    // save→load the .arp and confirm replay_seek on the LOADED take reproduces
    // the exact same frame as replay_seek on the in-memory take (proves the
    // disk format + the deterministic replay path).
    // -----------------------------------------------------------------------
    sess->unserialize(snap);
    sess->record_start();
    for (int i = 0; i < frames; ++i) {
        sess->set_input(0, (i % 16 < 2) ? START : 0);
        sess->step();
    }
    sess->record_stop();
    ayther::AytherRecording take = sess->take_recording();

    const char* arp = "smoke_take.arp";
    const bool saved  = take.save(arp);
    auto       loaded = ayther::AytherRecording::load(arp);
    const bool meta_ok = loaded
                       && loaded->frame_count() == static_cast<uint32_t>(frames)
                       && loaded->game_id == take.game_id;

    // R7c: per-frame sprite-hash history must survive the .arp round-trip.
    const bool hash_ok = loaded
                       && loaded->sprite_hashes == take.sprite_hashes
                       && loaded->hash_offsets  == take.hash_offsets;
    std::printf("[%s] hash history: %zu hashes, %zu offsets (round-trip %s)\n",
                hash_ok ? "ok" : "FAIL", take.sprite_hashes.size(),
                take.hash_offsets.size(), hash_ok ? "exact" : "MISMATCH");

    bool replay_ok = false;
    if (meta_ok && frames > 0) {
        const uint32_t target = static_cast<uint32_t>(frames - 1);
        const ayther::FrameView* a = sess->replay_seek(take, target);
        const uint64_t ha = a ? hash_fb(*a, 0) : 0;
        const ayther::FrameView* b = sess->replay_seek(*loaded, target);
        const uint64_t hb = b ? hash_fb(*b, 0) : 0;
        replay_ok = (ha == hb) && (ha != 0);
        std::printf("[%s] recording round-trip: %u frames, replay frame %u  "
                    "mem=%016llx disk=%016llx\n",
                    replay_ok ? "ok" : "FAIL", take.frame_count(), target,
                    static_cast<unsigned long long>(ha),
                    static_cast<unsigned long long>(hb));
    } else {
        std::printf("[FAIL] recording: save=%d load+meta=%d\n",
                    static_cast<int>(saved), static_cast<int>(meta_ok));
    }
    std::remove(arp);

    // -----------------------------------------------------------------------
    // Audio events (C-A2): analizar la toma grabada → eventos por canal a través
    // de la sesión (recording-céntrico). Verifica que corre end-to-end y es
    // determinista (re-analizar la misma toma da el mismo set). Con core stock
    // (sin id 0x109) da 0 eventos — sólo se exige el determinismo, no que haya >0.
    // -----------------------------------------------------------------------
    bool audio_ev_ok = true, sub_ok = true;
    {
        // Grabar una toma corta de GAMEPLAY (RIGHT + salto; NO START, que pausa)
        // desde el estado vivo ya avanzado, para que tenga key-ons reales, y
        // analizarla. Nota: una toma que arranca de un savestate a mitad de canción
        // no detecta las notas YA sonando en el frame 0 (sólo las que se disparan
        // dentro de la toma) — por eso conviene gameplay activo, no un tramo quieto.
        constexpr uint16_t RIGHT = 1u << 7, JUMP = 1u << 8;
        sess->record_start();
        for (int i = 0; i < 200; ++i) {
            sess->set_input(0, (i % 24 < 4) ? (RIGHT | JUMP) : RIGHT);
            sess->step();
        }
        sess->record_stop();
        ayther::AytherRecording atake = sess->take_recording();

        const uint32_t ne1 = sess->analyze_audio_events(atake);
        const AytherAudioEvent* evs = sess->audio_events();
        const uint64_t sig0 = (ne1 && evs) ? evs[0].signature : 0;
        const uint32_t ne2 = sess->analyze_audio_events(atake);   // re-analizar = mismo set (determinista)
        audio_ev_ok = (ne1 == ne2);
        std::printf("[%s] audio events (C-A2): %u detected (re-analyze=%u) sig0=%016llx\n",
                    audio_ev_ok ? "ok" : "FAIL", ne1, ne2,
                    static_cast<unsigned long long>(sig0));

        // -- C-A3b: sustitución por evento ------------------------------------
        // Asignar la firma MÁS recurrente a un asset, activar el preview, y
        // reproducir la toma verificando que FrameView.audio_mute_mask coincide
        // EXACTAMENTE con la ventana [start,end] de las ocurrencias de esa firma
        // (el bit del canal correcto). El mute real (que baja la energía) ya lo
        // prueba audio_mute_smoke; acá se verifica la LÓGICA de qué mutear.
        if (ne1 > 0 && evs) {
            std::unordered_map<uint64_t, int> freq;
            for (uint32_t i = 0; i < ne1; ++i) freq[evs[i].signature]++;
            uint64_t best = 0; int bestn = 0;
            for (const auto& kv : freq) if (kv.second > bestn) { bestn = kv.second; best = kv.first; }

            sess->assign_audio_event(best, "test_hd.ogg");
            sess->set_audio_substitution_preview(true);

            int frames_in_win = 0, frames_masked = 0, mismatches = 0, starts = 0;
            for (uint32_t f = 0; f < atake.frame_count(); ++f) {
                const ayther::FrameView* v = sess->replay_seek(atake, f);
                uint16_t expect = 0;
                for (uint32_t i = 0; i < ne1; ++i)
                    if (evs[i].signature == best && f >= evs[i].start_frame && f <= evs[i].end_frame)
                        expect |= (evs[i].chip == 0) ? uint16_t(1u << evs[i].channel)
                                                     : uint16_t(1u << (6 + evs[i].channel));
                if (!v) continue;
                if (expect) ++frames_in_win;
                if (v->audio_mute_mask) ++frames_masked;
                if (v->audio_mute_mask != expect) ++mismatches;
                starts += v->audio_active_sub_count;  // suma de subs activos (incluye is_start)
            }
            sess->set_audio_substitution_preview(false);

            const bool mask_ok = (mismatches == 0) && (frames_in_win > 0) && (frames_masked == frames_in_win);
            std::printf("[%s] event-sub (C-A3b): sig %016llx x%d → mask en %d/%d frames de ventana"
                        " (mismatch=%d, subs activos=%d)\n",
                        mask_ok ? "ok" : "FAIL", (unsigned long long)best, bestn,
                        frames_masked, frames_in_win, mismatches, starts);

            // -- C-A5: round-trip de audio_events.toml ------------------------
            const std::string toml = sess->audio_events_toml();
            const bool toml_has = toml.find("[[event]]") != std::string::npos
                               && toml.find("test_hd.ogg") != std::string::npos;
            sess->clear_audio_event_assignments();
            const bool cleared = sess->audio_event_assignment_count() == 0;
            sess->load_audio_events_toml(toml.c_str());
            const bool reloaded = sess->audio_event_assignment_count() == 1;
            const bool persist_ok = toml_has && cleared && reloaded;
            std::printf("[%s] event-sub persist (C-A5): toml=%zuB has=%d cleared=%d reloaded=%d\n",
                        persist_ok ? "ok" : "FAIL", toml.size(), toml_has, cleared, reloaded);
            sess->clear_audio_event_assignments();

            // -- C-A4 handoff: exportar el audio base del evento a WAV ---------
            uint32_t es = 0, ee = 0; bool ef = false;
            for (uint32_t i = 0; i < ne1 && !ef; ++i)
                if (evs[i].signature == best) { es = evs[i].start_frame; ee = evs[i].end_frame; ef = true; }
            bool export_ok = false;
            if (ef && sess->export_audio_event_wav(atake, es, ee, "smoke_event.wav")) {
                if (FILE* wf = std::fopen("smoke_event.wav", "rb")) {
                    char hdr[4] = {0};
                    const size_t got = std::fread(hdr, 1, 4, wf);
                    std::fseek(wf, 0, SEEK_END);
                    const long sz = std::ftell(wf);
                    std::fclose(wf);
                    export_ok = (got == 4) && (std::memcmp(hdr, "RIFF", 4) == 0) && (sz > 44);
                }
                std::remove("smoke_event.wav");
            }
            std::printf("[%s] event-export (C-A4): wav base %s\n",
                        export_ok ? "ok" : "FAIL", export_ok ? "válido (RIFF+datos)" : "vacío/inválido");

            sub_ok = mask_ok && persist_ok && export_ok;
        }
    }

    // -----------------------------------------------------------------------
    // C-A4 paso 3: sustitución de audio EN VIVO (runtime). Vuelve a un estado de
    // gameplay vivo, enciende el runtime sub, junta las firmas activas (sin
    // asignar → sin mute), las asigna y verifica que el mute reaccione en vivo.
    // -----------------------------------------------------------------------
    bool live_ok = true;
    {
        // Sigue VIVO desde el estado actual (gameplay; la toma analizada era
        // gameplay). NO usamos `snap`: la warmup tapea START (pausa) y cae en un
        // frame pausado → sin audio. Unas vueltas de gameplay para tener audio.
        constexpr uint16_t RIGHT = 1u << 7, JUMP = 1u << 8;
        sess->set_audio_runtime_substitution(true);
        for (int i = 0; i < 60; ++i) { sess->set_input(0, (i % 24 < 4) ? (RIGHT | JUMP) : RIGHT); sess->step(); }
        std::unordered_map<uint64_t, int> seen;
        for (int i = 0; i < 90; ++i) {                 // warm: juntar firmas activas
            sess->set_input(0, RIGHT);
            sess->step();
            AytherAudioActive act[64];
            const uint32_t na = sess->audio_live_active(act, 64);
            for (uint32_t k = 0; k < na; ++k) seen[act[k].signature]++;
        }
        for (const auto& kv : seen) sess->assign_audio_event(kv.first, "live_test.ogg");
        uint32_t max_mask = 0;
        for (int i = 0; i < 60; ++i) {                 // ¿el mute reacciona en vivo?
            sess->set_input(0, RIGHT);
            const ayther::FrameView& v = sess->step();
            if (v.audio_mute_mask > max_mask) max_mask = v.audio_mute_mask;
        }
        sess->set_audio_runtime_substitution(false);
        sess->clear_audio_event_assignments();
        live_ok = seen.empty() || (max_mask != 0);     // con firmas asignadas activas → muteó
        std::printf("[%s] runtime live-sub (C-A4 p3): %zu firmas vistas, max_mask=0x%03x\n",
                    live_ok ? "ok" : "FAIL", seen.size(), max_mask);
    }

    // -----------------------------------------------------------------------
    // SECUENCIAS EN VIVO (Capturar): una sub de set_audio_sequence_subs debe
    // disparar por el key-on real del detector (rising-edge de su firma
    // disparadora) y range-mutear sus canales — sin toma analizada de por medio
    // y sin ninguna asignación per-firma (la máscara solo puede venir de la
    // ventana de la Secuencia).
    // -----------------------------------------------------------------------
    bool seq_live_ok = true;
    {
        constexpr uint16_t RIGHT = 1u << 7;
        sess->set_audio_runtime_substitution(true);
        std::unordered_map<uint64_t, int> seen;
        for (int i = 0; i < 90; ++i) {                 // juntar firmas activas
            sess->set_input(0, RIGHT);
            sess->step();
            AytherAudioActive act[64];
            const uint32_t na = sess->audio_live_active(act, 64);
            for (uint32_t k = 0; k < na; ++k) seen[act[k].signature]++;
        }
        uint32_t max_mask = 0;
        if (!seen.empty()) {
            uint64_t trig = 0; int best = -1;          // la más recurrente re-keyea seguro
            for (const auto& kv : seen)
                if (kv.second > best) { best = kv.second; trig = kv.first; }
            std::vector<ayther::AytherSession::AudioSeqSub> subs(1);
            subs[0].trigger_signature = trig;
            subs[0].duration_frames   = 30;
            subs[0].span_frames       = 30;
            subs[0].channel_mask      = 0x3ff;
            subs[0].key               = 0xC0FFEEull;
            subs[0].asset             = "seq_live_test.ogg";
            subs[0].signatures        = { trig };
            sess->set_audio_sequence_subs(std::move(subs));
            for (int i = 0; i < 120; ++i) {
                sess->set_input(0, RIGHT);
                const ayther::FrameView& v = sess->step();
                if (v.audio_mute_mask > max_mask) max_mask = v.audio_mute_mask;
            }
            sess->set_audio_sequence_subs({});
            seq_live_ok = (max_mask != 0);
        }
        sess->set_audio_runtime_substitution(false);
        std::printf("[%s] secuencia live (Capturar): %zu firmas, max_mask=0x%03x\n",
                    seq_live_ok ? "ok" : "FAIL", seen.size(), max_mask);
    }

    const bool det  = (first_div < 0);
    const bool live = (max_tile > 0) || (fb_present > 0);   // pipeline produced real data
    const bool ok   = det && live && !snap.empty() && saved && meta_ok && replay_ok && hash_ok
                      && true && audio_ev_ok && sub_ok && live_ok && seq_live_ok;

    std::printf("\n--- results ---\n");
    std::printf("fb hash  A[last]=%016llx  B[last]=%016llx\n",
                static_cast<unsigned long long>(seqA.back()),
                static_cast<unsigned long long>(seqB.back()));
    if (det)   std::printf("[ok] DETERMINISTIC through the facade — %d frames identical after unserialize\n", frames);
    else       std::printf("[FAIL] divergence at frame %d\n", first_div);
    if (!live) std::printf("[FAIL] pipeline produced no data (no framebuffer, no occurrences)\n");

    std::printf("\n=== AytherSession smoke: %s ===\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
