// ---------------------------------------------------------------------------
// anim_smoke — valida el WIRING de Animaciones C-S2 en AytherSession: por la
// pose que el juego muestra este frame, produce_frame emite el frame HD del
// sheet (FrameView.anim_frames) — Nivel 0 (Pop, bbox observado) y Nivel 1
// (tween geométrico del transform entre keyframes).
//
// Headless: valida la cadena define → resolve → publicación (los AnimHdFrame
// con su sub-rect y su dst). El pase de render (VkSprite::draw_anim, mismo
// pipeline con sub-rect UV en push constants) se verifica visualmente en el Lab.
//
//   Build:  -DAYTHER_BUILD_SPIKE=ON  →  target anim_smoke
//   Run:    bin/anim_smoke <core_vram.dll> <sonic2.md>
// ---------------------------------------------------------------------------
#include "ayther_session.h"
#include "ayther_components_toml.h"   // bake_animations_toml (modo --bake-pack)

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using ayther::AytherSession;
using ayther::FrameView;
using ayther::HdPose;

static constexpr uint32_t OFF_SONIC_X = 0xB008;
static constexpr uint32_t OFF_SONIC_Y = 0xB00C;

static constexpr uint16_t BTN_START = 1u << 3;
static constexpr uint16_t BTN_RIGHT = 1u << 7;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
            "anim_smoke — session-level wiring check for C-S2 HD animation playback\n"
            "usage: %s <core.dll> <rom>\n", argv[0]);
        return 2;
    }
    AytherSession::Config cfg;
    cfg.core_path = argv[1]; cfg.rom_path = argv[2]; cfg.enable_audio = false;
    auto r = AytherSession::create(cfg);
    if (!r) { std::fprintf(stderr, "[FAIL] create: %s\n", r.error.message.c_str()); return 1; }
    std::unique_ptr<AytherSession>& s = *r;
    std::printf("=== anim_smoke ===\ncore: %s\nrom : %s\n\n", argv[1], argv[2]);

    int fail = 0;
    auto check = [&](bool ok, const char* m) {
        std::printf("[%s] %s\n", ok ? "ok" : "FAIL", m);
        if (!ok) ++fail;
    };

    // Warm-up hasta gameplay.
    int reached = -1;
    for (int i = 0; i < 1200; ++i) {
        s->set_input(0, (i % 32 == 0) ? BTN_START : 0);
        s->step();
        if (s->ram_u16(OFF_SONIC_X) != 0 && s->ram_u16(OFF_SONIC_Y) != 0) { reached = i; break; }
    }
    check(reached >= 0, "gameplay alcanzado");
    if (reached < 0) { std::printf("\nFALLÓ\n"); return 1; }
    for (int i = 0; i < 180; ++i) { s->set_input(0, BTN_RIGHT); s->step(); }

    // Descubrir un clip real: correr y juntar por anim_group_id las poses vistas
    // (el ciclo de correr de Sonic cicla varias poses bajo un mismo grupo).
    std::map<uint64_t, std::set<uint64_t>> groups;   // clip → poses vistas
    const FrameView* fv = nullptr;
    for (int i = 0; i < 180; ++i) {
        s->set_input(0, BTN_RIGHT);
        fv = &s->step();
        for (uint32_t k = 0; k < fv->sprite_occ_count; ++k) {
            const auto& o = fv->sprite_occs[k];
            if (o.anim_group_id != 0) groups[o.anim_group_id].insert(o.hash);
        }
    }
    uint64_t clip = 0; size_t nposes = 0;
    for (const auto& [g, poses] : groups)
        if (poses.size() > nposes) { clip = g; nposes = poses.size(); }
    std::printf("[..] grupos animados vistos: %zu; clip elegido=%016llx con %zu poses\n",
                groups.size(), (unsigned long long)clip, nposes);
    check(clip != 0 && nposes >= 2, "hay un clip real con >=2 poses (ciclo de animación)");
    if (!clip) { std::printf("\nFALLÓ\n"); return 1; }

    // -- Nivel 0 (Pop): definir la animación HD del clip ------------------------
    std::vector<HdPose> poses;
    std::map<uint64_t, uint32_t> pose_slot;   // hash → índice (para validar src)
    for (uint64_t h : groups[clip]) {
        HdPose p;
        p.pose  = h;
        p.src_x = float(poses.size()) * 64.0f;  p.src_y = 0.0f;
        p.src_w = 64.0f;                        p.src_h = 64.0f;
        p.anchor = { 500.0f + float(poses.size()) * 100.0f, 300.0f, 64.0f, 64.0f };
        p.duration_ticks = 6;
        pose_slot[h] = uint32_t(poses.size());
        poses.push_back(p);
    }
    s->define_animation(clip, "sheets/sonic_run.png", poses.data(),
                        uint32_t(poses.size()), /*tween_level=*/0);
    check(s->animation_count() == 1, "define_animation registra el clip");

    // Correr y validar: el frame HD sale EN FASE (la pose en pantalla decide el
    // sub-rect) y en el bbox observado (Pop).
    int hits = 0, in_bbox = 0, src_ok = 0;
    for (int i = 0; i < 120; ++i) {
        s->set_input(0, BTN_RIGHT);
        fv = &s->step();
        if (fv->anim_frame_count == 0) continue;
        ++hits;
        for (uint32_t a = 0; a < fv->anim_frame_count; ++a) {
            const auto& f = fv->anim_frames[a];
            if (std::strcmp(f.asset, "sheets/sonic_run.png") != 0) continue;
            // ¿Corresponde a una occurrence de una pose del clip este frame?
            // (Por HASH de pose — el anim_group_id churnea con la ventana del
            // grouper, por eso el player también resuelve por pose.)
            for (uint32_t k = 0; k < fv->sprite_occ_count; ++k) {
                const auto& o = fv->sprite_occs[k];
                if (!pose_slot.count(o.hash)) continue;
                if (f.dst_x == float(o.screen_x) && f.dst_y == float(o.screen_y)) {
                    ++in_bbox;
                    if (f.src_x == float(pose_slot[o.hash]) * 64.0f) ++src_ok;
                    break;
                }
            }
        }
    }
    std::printf("[..] frames con anim HD: %d; en bbox: %d; sub-rect de la pose: %d\n",
                hits, in_bbox, src_ok);
    check(hits > 30, "la animación HD emite frames mientras el clip está en pantalla");
    check(in_bbox > 30, "Nivel 0 (Pop): el frame HD cae en el bbox observado");
    check(src_ok > 30, "el sub-rect del sheet corresponde a la pose EN PANTALLA (en fase)");

    // -- Modo bake (pasada visual): hornear un pack .ay con la animación REAL
    //    descubierta (poses del clip → frames del sheet) para que el PLAYER
    //    (ayther_runtime --pack) la reproduzca en pantalla vía draw_anim.
    //    anim_smoke <core> <rom> --bake-pack <out.ay> <sheet.png>
    for (int i = 3; i + 2 < argc + 1 && i < argc; ++i) {
        if (std::strcmp(argv[i], "--bake-pack") != 0 || i + 2 >= argc) continue;
        const char* out_ae  = argv[i + 1];
        const char* sheetfs = argv[i + 2];
        // Re-definir con el nombre de entry del pack (basename del sheet).
        std::string entry(sheetfs);
        const size_t slash = entry.find_last_of("/\\");
        if (slash != std::string::npos) entry = entry.substr(slash + 1);
        s->define_animation(clip, entry, poses.data(), uint32_t(poses.size()), 0);
        const std::string anim_toml =
            ayther::bake_animations_toml(s->animation_definitions());
        const std::string manifest =
            "[pack]\nname = \"anim_visual\"\nversion = \"0.0.1\"\n"
            "game_id = \"anim_visual\"\nayther_min = \"0.8.0\"\n";
        AytherPackBuilder* b = ayther_pack_builder_new();
        ayther_pack_builder_add_bytes(b, "manifest.toml",
            reinterpret_cast<const uint8_t*>(manifest.data()), manifest.size());
        ayther_pack_builder_add_bytes(b, "animations.toml",
            reinterpret_cast<const uint8_t*>(anim_toml.data()), anim_toml.size());
        const bool fok = ayther_pack_builder_add_file(b, entry.c_str(), sheetfs);
        char err[256] = {};
        const bool built = fok &&
            ayther_pack_builder_finish(b, /*sign=*/true, out_ae, err, sizeof(err));
        ayther_pack_builder_free(b);
        check(built, "bake: pack visual horneado (animations.toml + sheet)");
        std::printf("[..] pack visual: %s (%zu poses → frames de %s)\n",
                    out_ae, poses.size(), entry.c_str());
        std::printf("\n%s (%d fallos)\n", fail ? "FALLÓ" : "OK", fail);
        return fail ? 1 : 0;   // en modo bake no corren las fases L1/undefine
    }

    // -- Nivel 1 (tween geométrico): el dst sale de los anchors interpolados ----
    s->define_animation(clip, "sheets/sonic_run.png", poses.data(),
                        uint32_t(poses.size()), /*tween_level=*/1);
    std::set<int> dxs;
    int anchored = 0, total = 0;
    for (int i = 0; i < 90; ++i) {
        s->set_input(0, BTN_RIGHT);
        fv = &s->step();
        for (uint32_t a = 0; a < fv->anim_frame_count; ++a) {
            const auto& f = fv->anim_frames[a];
            if (std::strcmp(f.asset, "sheets/sonic_run.png") != 0) continue;
            ++total;
            // dst en el espacio de los anchors (x >= 400), no en el bbox (~150).
            if (f.dst_x >= 400.0f) ++anchored;
            dxs.insert(int(f.dst_x));
        }
    }
    std::printf("[..] frames L1: %d; con dst del tween: %d; valores distintos de dst_x: %zu\n",
                total, anchored, dxs.size());
    check(total > 30 && anchored == total,
          "Nivel 1: el dst sale del transform tweeneado (anchors), no del bbox");
    check(dxs.size() >= 3, "el tween INTERPOLA (el dst desliza entre keyframes)");

    // -- Limpieza ----------------------------------------------------------------
    s->undefine_animation(clip);
    fv = &s->step();
    check(s->animation_count() == 0 && fv->anim_frame_count == 0,
          "undefine_animation limpia el clip y los frames publicados");

    std::printf("\n%s (%d fallos)\n", fail ? "FALLÓ" : "OK", fail);
    return fail ? 1 : 0;
}
