// ---------------------------------------------------------------------------
// audio_events_smoke — valida el WIRING de Audios C-A2 en AytherSession:
// resolve_audio_events corre el detector (C-A1) sobre el historial de audio de
// una toma real (.arp v7) y assign_audio_event resuelve las ventanas de
// sustitución (rango-mute + trigger) que produce_frame consulta por frame.
//
// Headless (sin device de audio): valida la cadena detector → asignación →
// ventanas resueltas (audio_event_subs). La salida audible (play_event_hd /
// rango-mute en vivo) usa el mismo camino ya validado del AudioPlayer
// (get_wav WAV/OGG/FLAC + streams SDL) y se verifica a oído en el Lab.
//
//   Build:  -DAYTHER_BUILD_SPIKE=ON  →  target audio_events_smoke
//   Run:    bin/audio_events_smoke <core_vram.dll> <sonic2.md>
// ---------------------------------------------------------------------------
#include "ayther_session.h"
#include "ayther_recording.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

using ayther::AytherSession;

static constexpr uint32_t OFF_SONIC_X = 0xB008;
static constexpr uint32_t OFF_SONIC_Y = 0xB00C;

static constexpr uint16_t BTN_START = 1u << 3;
static constexpr uint16_t BTN_RIGHT = 1u << 7;
static constexpr uint16_t BTN_JUMP  = 1u << 0;   // MD B = salto (SFX extra)

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
            "audio_events_smoke — session-level wiring check for C-A2 audio events\n"
            "usage: %s <core.dll> <rom>\n", argv[0]);
        return 2;
    }
    AytherSession::Config cfg;
    cfg.core_path = argv[1]; cfg.rom_path = argv[2]; cfg.enable_audio = false;
    auto r = AytherSession::create(cfg);
    if (!r) { std::fprintf(stderr, "[FAIL] create: %s\n", r.error.message.c_str()); return 1; }
    std::unique_ptr<AytherSession>& s = *r;
    std::printf("=== audio_events_smoke ===\ncore: %s\nrom : %s\n\n", argv[1], argv[2]);

    int fail = 0;
    auto check = [&](bool ok, const char* m) {
        std::printf("[%s] %s\n", ok ? "ok" : "FAIL", m);
        if (!ok) ++fail;
    };

    // Warm-up hasta gameplay, luego grabar una toma con sonido (música de EHZ
    // corriendo + SFX de saltos).
    int reached = -1;
    for (int i = 0; i < 1200; ++i) {
        s->set_input(0, (i % 32 == 0) ? BTN_START : 0);
        s->step();
        if (s->ram_u16(OFF_SONIC_X) != 0 && s->ram_u16(OFF_SONIC_Y) != 0) { reached = i; break; }
    }
    check(reached >= 0, "gameplay alcanzado");
    if (reached < 0) { std::printf("\nFALLÓ\n"); return 1; }
    for (int i = 0; i < 120; ++i) { s->set_input(0, BTN_RIGHT); s->step(); }

    s->record_start();
    for (int i = 0; i < 600; ++i) {
        uint16_t in = BTN_RIGHT;
        if (i % 90 < 6) in |= BTN_JUMP;   // saltos periódicos (SFX sobre la música)
        s->set_input(0, in);
        s->step();
    }
    s->record_stop();
    ayther::AytherRecording take = s->take_recording();
    std::printf("[..] toma: %u frames, %zu hashes de audio\n",
                take.frame_count(), take.audio_hashes.size());
    check(take.frame_count() == 600, "la toma tiene los frames grabados");
    check(!take.audio_hashes.empty() && take.audio_offsets.size() == take.frame_count() + 1,
          "la toma trae el historial de audio (.arp v7, CSR por frame)");

    // -- Detección de eventos --------------------------------------------------
    const size_t n = s->resolve_audio_events(take);
    std::printf("[..] eventos detectados: %zu\n", n);
    check(n >= 1, "el detector encontró >=1 evento en la toma");
    size_t ecount = 0;
    const AytherAudioEvent* evs = s->audio_events(&ecount);
    check(ecount == n && (n == 0 || evs != nullptr), "audio_events expone los detectados");
    bool ranges_ok = n > 0;
    uint32_t widest = 0;
    for (size_t i = 0; i < ecount; ++i) {
        std::printf("     evento sig=%016llx frames [%u..%u] chip=%u\n",
                    (unsigned long long)evs[i].signature,
                    evs[i].start_frame, evs[i].end_frame, evs[i].chip);
        if (evs[i].end_frame < evs[i].start_frame ||
            evs[i].end_frame >= take.frame_count()) ranges_ok = false;
        if (evs[i].end_frame - evs[i].start_frame >
            evs[widest].end_frame - evs[widest].start_frame) widest = (uint32_t)i;
    }
    check(ranges_ok, "los rangos de los eventos caen dentro de la toma");
    if (!n) { std::printf("\nFALLÓ (%d)\n", fail); return 1; }

    // -- Asignar → ventanas resueltas -------------------------------------------
    const uint64_t sig = evs[widest].signature;
    const uint64_t e_start = evs[widest].start_frame, e_end = evs[widest].end_frame;
    s->assign_audio_event(sig, "audio/music_hd.ogg", /*looping=*/true);

    size_t nsubs = 0;
    const AytherAudioEventSub* subs = s->audio_event_subs(&nsubs);
    std::printf("[..] ventanas resueltas tras asignar: %zu\n", nsubs);
    check(nsubs >= 1, "assign_audio_event resuelve >=1 ventana de sustitución");
    bool win_ok = false, asset_ok = false, loop_ok = false;
    for (size_t i = 0; i < nsubs; ++i) {
        if (subs[i].signature != sig) continue;
        if (subs[i].start_frame == e_start && subs[i].end_frame == e_end) win_ok = true;
        asset_ok = std::strcmp(subs[i].asset_path, "audio/music_hd.ogg") == 0;
        loop_ok  = subs[i].looping != 0;
    }
    check(win_ok,  "la ventana coincide con el rango del evento (rango-mute alineado)");
    check(asset_ok, "la ventana lleva el asset asignado");
    check(loop_ok,  "la ventana conserva el flag looping");

    // -- Enumeración para Deliver + desasignar -----------------------------------
    auto assigns = s->audio_event_assignments();
    check(assigns.size() == 1 && assigns[0].signature == sig &&
          assigns[0].asset == "audio/music_hd.ogg" && assigns[0].looping,
          "audio_event_assignments enumera la asignación (audio_events.toml)");

    s->assign_audio_event(sig, "", false);   // "" desasigna
    subs = s->audio_event_subs(&nsubs);
    check(nsubs == 0, "desasignar limpia las ventanas");
    check(s->audio_event_assignments().empty(), "desasignar limpia la enumeración");

    // Re-resolver con otra toma vacía no debe dejar ventanas colgadas.
    s->assign_audio_event(sig, "audio/music_hd.ogg", true);
    ayther::AytherRecording empty_take;
    s->resolve_audio_events(empty_take);
    subs = s->audio_event_subs(&nsubs);
    check(nsubs == 0, "cambiar a una toma sin eventos limpia las ventanas");

    std::printf("\n%s (%d fallos)\n", fail ? "FALLÓ" : "OK", fail);
    return fail ? 1 : 0;
}
