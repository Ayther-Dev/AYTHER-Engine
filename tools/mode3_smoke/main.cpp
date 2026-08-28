// ---------------------------------------------------------------------------
// mode3_smoke — valida el WIRING de Modo 3 en AytherSession (produce_frame →
// Mode3Resolver → FrameView), contra Sonic 2 real.
//
// tools/mode3_spike ya validó el CAMINO (lectura de cámara del VDP + perfil +
// assign_sprites) a nivel runner/core. Este smoke valida la CAPA de sesión:
//   1. load_game_profile carga el perfil TOML (sonic2.toml).
//   2. produce_frame captura la cámara del plano A (Hscroll/VSRAM) con el signo
//      correcto y llama al resolver → FrameView.entity_instances.
//   3. Oráculo INDEPENDIENTE: parado (cámara quieta), la posición de pantalla
//      esperada del jugador es world_pos(RAM) − Camera_pos(RAM) — dos lecturas
//      de work RAM que NO pasan por el VDP ni por el resolver. La instancia
//      localizada debe contener ese punto (si el signo del scroll estuviera mal,
//      el bbox caería en el lado opuesto de la pantalla).
//   4. assign_kind publica un AytherSpriteSub por instancia en entity_subs
//      (lane de sprites del renderer); "" desasigna; load_game_profile("")
//      descarga el perfil y limpia los resultados.
//
//   Build:  -DAYTHER_BUILD_SPIKE=ON  →  target mode3_smoke
//   Run:    bin/mode3_smoke <core_vram.dll> <sonic2.md> [--profile P] [--warmup N]
// ---------------------------------------------------------------------------
#include "ayther_session.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#ifndef AYTHER_SOURCE_DIR
#define AYTHER_SOURCE_DIR "."
#endif

using ayther::AytherSession;
using ayther::FrameView;

// Sonic 2 (World) — offsets 68k en work RAM (los del perfil + espejo del spike).
static constexpr uint32_t OFF_SONIC_X  = 0xB008;
static constexpr uint32_t OFF_SONIC_Y  = 0xB00C;
static constexpr uint32_t OFF_CAMERA_X = 0xEE00;
static constexpr uint32_t OFF_CAMERA_Y = 0xEE04;

static constexpr uint16_t BTN_START = 1u << 3;
static constexpr uint16_t BTN_RIGHT = 1u << 7;   // RETRO_DEVICE_ID_JOYPAD_RIGHT

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
            "mode3_smoke — session-level wiring check for Mode 3 (RAM anchoring)\n"
            "usage: %s <core.dll> <rom> [--profile P] [--warmup N]\n", argv[0]);
        return 2;
    }
    std::string core = argv[1], rom = argv[2];
    std::string profile = AYTHER_SOURCE_DIR "/tools/mode3_spike/sonic2.toml";
    int warmup = 1200;
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--profile") && i + 1 < argc) profile = argv[++i];
        else if (!std::strcmp(argv[i], "--warmup") && i + 1 < argc) warmup = std::atoi(argv[++i]);
    }

    AytherSession::Config cfg;
    cfg.core_path = core; cfg.rom_path = rom; cfg.enable_audio = false;
    auto r = AytherSession::create(cfg);
    if (!r) { std::fprintf(stderr, "[FAIL] create: %s\n", r.error.message.c_str()); return 1; }
    std::unique_ptr<AytherSession>& s = *r;
    std::printf("=== mode3_smoke ===\ncore: %s\nrom : %s\nprof: %s\n\n",
                core.c_str(), rom.c_str(), profile.c_str());

    int fail = 0;
    auto check = [&](bool ok, const char* m) {
        std::printf("[%s] %s\n", ok ? "ok" : "FAIL", m);
        if (!ok) ++fail;
    };

    // -- Perfil -------------------------------------------------------------
    auto pr = s->load_game_profile(profile);
    check(bool(pr), "load_game_profile carga el perfil TOML");
    check(s->has_game_profile(), "has_game_profile refleja el perfil cargado");
    if (!pr) { std::printf("\nFALLÓ\n"); return 1; }

    // -- Warm-up adaptativo: SEGA→título→EHZ (el jugador spawnea = X/Y != 0) --
    int reached = -1;
    for (int i = 0; i < warmup; ++i) {
        s->set_input(0, (i % 32 == 0) ? BTN_START : 0);
        s->step();
        if (s->ram_u16(OFF_SONIC_X) != 0 && s->ram_u16(OFF_SONIC_Y) != 0) { reached = i; break; }
    }
    check(reached >= 0, "gameplay alcanzado (el jugador spawneó)");
    if (reached < 0) { std::printf("\nFALLÓ\n"); return 1; }
    std::printf("[..] gameplay en el frame %d de warm-up\n", reached);

    // Correr a la derecha para acumular scroll grande (el test de signo se vuelve
    // sensible; la title card del acto se come los primeros ~100 frames), luego
    // soltar y esperar QUIETO: la cámara se asienta (el Hscroll latcheado del VDP
    // coincide EXACTO con Camera_pos de RAM) y expira el parpadeo de invencibilidad
    // si el jugador se golpeó con algo durante la corrida a ciegas (parpadeando,
    // sus sprites desaparecen de la SAT en frames alternos).
    for (int i = 0; i < 420; ++i) { s->set_input(0, BTN_RIGHT); s->step(); }
    s->set_input(0, 0);
    for (int i = 0; i < 180; ++i) s->step();
    const FrameView* fv = &s->step();

    const int cam_x = s->ram_u16(OFF_CAMERA_X), cam_y = s->ram_u16(OFF_CAMERA_Y);
    const int exp_x = (int)(int16_t)s->ram_u16(OFF_SONIC_X) - cam_x;   // pantalla esperada
    const int exp_y = (int)(int16_t)s->ram_u16(OFF_SONIC_Y) - cam_y;   // (ground truth RAM)
    std::printf("[..] cámara RAM=(%d,%d)  jugador esperado en pantalla=(%d,%d)\n",
                cam_x, cam_y, exp_x, exp_y);
    check(cam_x > 100, "la cámara scrolleó lejos del origen (test de signo sensible)");

    // -- Instancias localizadas ----------------------------------------------
    std::printf("[..] instancias=%u  sprites del frame=%u\n",
                fv->entity_instance_count, fv->sprite_occ_count);
    check(fv->entity_instance_count >= 1, "el perfil localizó >=1 instancia (entity_instances)");

    // Oráculo de posición: alguna instancia contiene el punto esperado del jugador
    // (world_pos(RAM) − Camera_pos(RAM), independiente del VDP y del resolver).
    // Se sondea una ventana de frames: el punto exacto del golpe/parpadeo o un
    // Tails oscilando cerca pueden robar UN frame puntual, no la ventana entera.
    int player = -1;
    for (int probe = 0; probe < 120 && player < 0; ++probe) {
        const int px = (int)(int16_t)s->ram_u16(OFF_SONIC_X) - (int)s->ram_u16(OFF_CAMERA_X);
        const int py = (int)(int16_t)s->ram_u16(OFF_SONIC_Y) - (int)s->ram_u16(OFF_CAMERA_Y);
        for (uint32_t i = 0; i < fv->entity_instance_count && player < 0; ++i) {
            const ayther::EntityInstance& e = fv->entity_instances[i];
            const int m = 16;   // margen: el bbox agrega sprites, el punto es el pivot
            if (px >= e.screen_x - m && px <= e.screen_x + (int)e.w_px + m &&
                py >= e.screen_y - m && py <= e.screen_y + (int)e.h_px + m)
                player = (int)i;
        }
        if (player < 0) fv = &s->step();
    }
    for (uint32_t i = 0; i < fv->entity_instance_count; ++i) {
        const ayther::EntityInstance& e = fv->entity_instances[i];
        std::printf("     id=0x%llX kind=%d bbox=(%d,%d %ux%u) sprites=%u\n",
                    (unsigned long long)e.id, e.kind, e.screen_x, e.screen_y,
                    e.w_px, e.h_px, e.sprite_count);
    }
    check(player >= 0, "una instancia contiene la posición esperada del jugador (signo de cámara OK)");
    check(player >= 0 && fv->entity_instances[player].sprite_count >= 1,
          "la instancia del jugador reclamó sprites SAT reales");

    // -- assign_kind → entity_subs (lane HD del renderer) ---------------------
    s->assign_kind("player", "dummy.png");
    fv = &s->step();
    std::printf("[..] entity_subs tras assign_kind=%u\n", fv->entity_sub_count);
    check(fv->entity_sub_count >= 1, "assign_kind publica >=1 AytherSpriteSub en entity_subs");
    bool sub_ok = fv->entity_sub_count >= 1;
    if (sub_ok) {
        const AytherSpriteSub& sub = fv->entity_subs[0];
        sub_ok = !std::strcmp(sub.asset_path, "dummy.png");
        check(sub_ok, "el sub lleva el asset asignado");
        // El sub se ubica en el bbox de ALGUNA instancia publicada este frame.
        bool at_instance = false;
        for (uint32_t i = 0; i < fv->entity_instance_count && !at_instance; ++i)
            at_instance = sub.screen_x == fv->entity_instances[i].screen_x &&
                          sub.screen_y == fv->entity_instances[i].screen_y;
        check(at_instance, "el sub está en el bbox de una instancia");
    }

    // -- Desasignar / descargar ----------------------------------------------
    s->assign_kind("player", "");
    fv = &s->step();
    check(fv->entity_sub_count == 0, "assign_kind(\"\") desasigna (0 subs)");
    check(fv->entity_instance_count >= 1, "las instancias siguen publicadas sin asset");

    auto pr2 = s->load_game_profile("");
    fv = &s->step();
    check(bool(pr2) && !s->has_game_profile(), "load_game_profile(\"\") descarga el perfil");
    check(fv->entity_instance_count == 0 && fv->entity_sub_count == 0,
          "sin perfil no se publican instancias ni subs");

    std::printf("\n%s (%d fallos)\n", fail ? "FALLÓ" : "OK", fail);
    return fail ? 1 : 0;
}
