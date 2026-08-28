// ---------------------------------------------------------------------------
// components_toml_smoke — round-trip de los TOML de Componentes SIN emulador:
// define/asigna → bake_*_toml → objetos frescos → parse_*_toml → comparar.
// Valida C-S4 (animations.toml) y C-A4 (audio_events.toml) a nivel formato;
// la carga desde un pack real la ejercita load_pack_into (ayther_session).
//
//   Build:  -DAYTHER_BUILD_SPIKE=ON  →  target components_toml_smoke
//   Run:    bin/components_toml_smoke      (sin argumentos)
// ---------------------------------------------------------------------------
#include "ayther_components_toml.h"
#include "ayther_session.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace ayther;

int main(int argc, char** argv) {
    int fail = 0;
    auto check = [&](bool ok, const char* m) {
        std::printf("[%s] %s\n", ok ? "ok" : "FAIL", m);
        if (!ok) ++fail;
    };
    std::printf("=== components_toml_smoke ===\n\n");

    // -- Animaciones: definir 2 clips (Pop y tween) ---------------------------
    AnimationPlayer src_anim;
    HdPose run[3];
    for (int i = 0; i < 3; ++i) {
        run[i].pose  = 0xAB00000000000000ull + uint64_t(i);
        run[i].src_x = float(i) * 64.0f; run[i].src_y = 0.0f;
        run[i].src_w = 64.0f;            run[i].src_h = 64.0f;
        run[i].anchor = { 100.0f + float(i) * 50.0f, 200.0f, 64.0f, 64.0f };
        run[i].duration_ticks = uint16_t(4 + i);
    }
    src_anim.define(0x1111u, "sheets/run.png", run, 3, /*tween*/1);
    HdPose idle;
    idle.pose = 0xCD00000000000001ull;
    idle.src_x = 0; idle.src_y = 64; idle.src_w = 48; idle.src_h = 48;
    idle.duration_ticks = 1;
    src_anim.define(0x2222u, "sheets/idle.png", &idle, 1, /*tween*/0);

    const std::string anim_toml = bake_animations_toml(src_anim.definitions());
    std::printf("[..] animations.toml horneado: %zu bytes\n", anim_toml.size());

    AnimationPlayer dst_anim;
    const size_t loaded = parse_animations_toml(anim_toml, dst_anim);
    check(loaded == 2 && dst_anim.clip_count() == 2, "parse carga los 2 clips");

    const auto a = src_anim.definitions(), b = dst_anim.definitions();
    bool same = a.size() == b.size();
    for (size_t i = 0; same && i < a.size(); ++i) {
        same = a[i].clip_id == b[i].clip_id && a[i].sheet == b[i].sheet &&
               a[i].tween_level == b[i].tween_level && a[i].poses.size() == b[i].poses.size();
        for (size_t j = 0; same && j < a[i].poses.size(); ++j) {
            const HdPose &p = a[i].poses[j], &q = b[i].poses[j];
            same = p.pose == q.pose && p.duration_ticks == q.duration_ticks &&
                   std::fabs(p.src_x - q.src_x) < 0.01f && std::fabs(p.src_y - q.src_y) < 0.01f &&
                   std::fabs(p.src_w - q.src_w) < 0.01f && std::fabs(p.src_h - q.src_h) < 0.01f &&
                   std::fabs(p.anchor.x - q.anchor.x) < 0.01f &&
                   std::fabs(p.anchor.w - q.anchor.w) < 0.01f;
        }
    }
    check(same, "las definiciones sobreviven el round-trip (clips, poses, rects, ticks)");

    // -- Audios: 2 asignaciones por evento ------------------------------------
    AudioEventSubstitution src_evt;
    src_evt.assign(0xF4652E7C251B9BC9ull, "audio/music_hd.ogg", true);
    src_evt.assign(0x0000000000000ABCull, "audio/voice_hi.wav", false);

    const std::string evt_toml = bake_audio_events_toml(src_evt.assignments());
    std::printf("[..] audio_events.toml horneado: %zu bytes\n", evt_toml.size());
    // El esquema debe ser el que parsea el core Rust ([[event]] + signature hex).
    check(evt_toml.find("[[event]]") != std::string::npos &&
          evt_toml.find("signature = \"0xf4652e7c251b9bc9\"") != std::string::npos &&
          evt_toml.find("loop = true") != std::string::npos,
          "el esquema matchea el parser del core ([[event]], signature hex, loop)");

    AudioEventSubstitution dst_evt;
    check(parse_audio_events_toml(evt_toml, dst_evt) == 2, "parse carga los 2 eventos");
    const auto ea = src_evt.assignments(), eb = dst_evt.assignments();
    bool esame = ea.size() == eb.size();
    for (size_t i = 0; esame && i < ea.size(); ++i)
        esame = ea[i].signature == eb[i].signature && ea[i].asset == eb[i].asset &&
                ea[i].looping == eb[i].looping;
    check(esame, "las asignaciones sobreviven el round-trip (firma, asset, loop)");

    // -- #218: members (firmas miembro de Secuencia) — mute selectivo ----------
    // El dialecto que emite pack_bake.cpp para las Secuencias: channels como
    // fallback + members con las firmas. parse_audio_event_members los lee;
    // los eventos SIN members no aparecen en el mapa (packs viejos → fallback).
    {
        const std::string seq_toml =
            "[[event]]\n"
            "signature = \"0x00000000000000aa\"\n"
            "asset = \"melodia_hd.wav\"\n"
            "channels = \"0x03c7\"\n"
            "duration = 180\n"
            "members = [\"0x00000000000000aa\", \"0x00000000000000bb\", "
            "\"0x00000000000000cc\"]\n"
            "\n"
            "[[event]]\n"
            "signature = \"0x00000000000000dd\"\n"
            "asset = \"sfx_hd.wav\"\n"
            "channels = \"0x0008\"\n";
        const auto mm = parse_audio_event_members(seq_toml);
        check(mm.size() == 1, "members: solo la entrada de Secuencia entra al mapa");
        const auto it = mm.find(0xAAull);
        check(it != mm.end() && it->second.size() == 3 &&
              it->second[0] == 0xAAull && it->second[1] == 0xBBull &&
              it->second[2] == 0xCCull,
              "members: las 3 firmas miembro parsean en orden");
        // El parser de asignaciones tolera el campo ajeno (back-compat).
        AudioEventSubstitution tol;
        check(parse_audio_events_toml(seq_toml, tol) == 2,
              "members no rompe el parser de asignaciones (campo tolerado)");
    }

    // -- E2E de pack (opcional, con core+rom): hornear un .ay con los dos TOML
    //    vía el pack builder (el camino del DeliverPanel) y verificar que la
    //    SESIÓN los carga al abrir el pack (load_pack_into).
    if (argc >= 4) {
        const std::string pack_path = std::string(argv[3]) + "/components_smoke.ay";
        const std::string manifest =
            "[pack]\nname = \"components_smoke\"\nversion = \"0.0.1\"\n"
            "game_id = \"smoke\"\nayther_min = \"0.8.0\"\n";
        AytherPackBuilder* b = ayther_pack_builder_new();
        ayther_pack_builder_add_bytes(b, "manifest.toml",
            reinterpret_cast<const uint8_t*>(manifest.data()), manifest.size());
        ayther_pack_builder_add_bytes(b, "animations.toml",
            reinterpret_cast<const uint8_t*>(anim_toml.data()), anim_toml.size());
        ayther_pack_builder_add_bytes(b, "audio_events.toml",
            reinterpret_cast<const uint8_t*>(evt_toml.data()), evt_toml.size());
        char err[256] = {};
        const bool built = ayther_pack_builder_finish(b, /*sign=*/true, pack_path.c_str(),
                                                      err, sizeof(err));   // dev-sign: sin firma el open la rechaza
        ayther_pack_builder_free(b);
        check(built, "el pack builder hornea el .ay con los dos TOML");
        if (built) {
            AytherSession::Config cfg;
            cfg.core_path = argv[1]; cfg.rom_path = argv[2];
            cfg.pack_path = pack_path; cfg.enable_audio = false;
            auto r = AytherSession::create(cfg);
            check(bool(r), "la sesión abre con el pack");
            if (r) {
                std::unique_ptr<AytherSession>& s = *r;
                check(s->animation_count() == 2,
                      "la sesión cargó las 2 animaciones del pack (animations.toml)");
                const auto loaded_evts = s->audio_event_assignments();
                check(loaded_evts.size() == 2 && loaded_evts[0].asset == "audio/voice_hi.wav",
                      "la sesión cargó las 2 asignaciones por evento (audio_events.toml)");
            }
        }
    } else {
        std::printf("[..] (sin core+rom: se omite el E2E de pack; usar "
                    "components_toml_smoke <core> <rom> <out_dir>)\n");
    }

    // -- ANIMACIÓN (#374): plane_sequences.toml + cadencia --------------------
    std::printf("\n-- Animación (plane_sequences.toml) --\n");
    {
        std::vector<PackPlaneSequence> src;
        PackPlaneSequence q;
        q.id = 0xA11CE0000000BEEFull;
        q.name = "Antorcha";
        // Paso 0: asset propio + cadencia. Paso 1: sin asset (usa el del set),
        // cadencia default. Paso 2: asset con `~` EN EL NOMBRE, para que el
        // parser tenga que leer la cadencia desde la derecha y no partir el
        // nombre por el primer `~` (nombres cortos 8.3 de Windows).
        q.steps.push_back({ 0x1000000000000001ull, "objetos/antorcha_a.png", 4 });
        q.steps.push_back({ 0x1000000000000002ull, "",                        0 });
        q.steps.push_back({ 0x1000000000000003ull, "objetos/ANTOR~1.png",     6 });
        src.push_back(q);
        // Un solo paso: NO se hornea (el Objeto solo ya lo cubre el plane set).
        PackPlaneSequence tiny;
        tiny.id = 0xDEADull;
        tiny.steps.push_back({ 0x1000000000000009ull, "", 0 });
        src.push_back(tiny);

        const std::string toml = bake_plane_sequences_toml(src);
        std::vector<PackPlaneSequence> back;
        const size_t n = parse_plane_sequences_toml(toml, back);
        check(n == 1 && back.size() == 1,
              "round-trip: se hornea 1 secuencia y la de UN paso se descarta");
        if (back.size() == 1) {
            const PackPlaneSequence& r = back[0];
            check(r.id == q.id && r.name == "Antorcha", "id y nombre sobreviven");
            check(r.steps.size() == 3, "los 3 pasos sobreviven");
            if (r.steps.size() == 3) {
                check(r.steps[0].set_id == q.steps[0].set_id &&
                      r.steps[0].asset  == q.steps[0].asset &&
                      r.steps[0].duration == 4,
                      "paso 0: set + asset + cadencia");
                check(r.steps[1].asset.empty() && r.steps[1].duration == 0,
                      "paso 1: sin asset y sin cadencia (se omiten los defaults)");
                check(r.steps[2].asset == "objetos/ANTOR~1.png" &&
                      r.steps[2].duration == 6,
                      "paso 2: la cadencia se lee DESDE LA DERECHA — un `~` en "
                      "el nombre del asset no lo parte");
            }
        }

        // Cadencia: durs {4, 0→8, 6} ⇒ ciclo de 18 frames.
        const uint16_t durs[3] = { 4, 0, 6 };
        const uint16_t kDef = 8;
        check(plane_sequence_total(durs, 3, kDef) == 18,
              "el ciclo dura 18 frames (4 + default 8 + 6)");
        const uint32_t want[18] = { 0,0,0,0, 1,1,1,1,1,1,1,1, 2,2,2,2,2,2 };
        bool cad_ok = true;
        for (uint32_t t = 0; t < 18; ++t)
            if (plane_sequence_step_at(durs, 3, t, kDef) != want[t]) cad_ok = false;
        check(cad_ok, "cada frame del ciclo cae en su paso (0×4 · 1×8 · 2×6)");
        check(plane_sequence_step_at(durs, 3, 17, kDef) == 2 &&
              plane_sequence_step_at(durs, 3, 0, kDef) == 0,
              "los bordes del ciclo: 17 es el último paso y 0 el primero");
    }

    std::printf("\n%s (%d fallos)\n", fail ? "FALLÓ" : "OK", fail);
    return fail ? 1 : 0;
}
