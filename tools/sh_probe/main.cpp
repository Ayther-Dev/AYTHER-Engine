// ---------------------------------------------------------------------------
// sh_probe — E3 (plan lab-estados-de-pose-plan.md): ¿alguna toma disponible
// EJERCITA el modo Shadow/Highlight del VDP? Antes de tocar el render (como el
// spike de E1), esto valida que la SEÑAL exista y sea usable.
//
// Replaya el .arp y por frame lee:
//   - reg 0x0C bit 3 = Shadow/Highlight enable (mode register 4).
//   - la distribución de prioridad de los sprites (occ.priority 0=baja/1=alta).
//
// El caso común de "sprite en sombra" = S/H activo + sprite de prioridad BAJA.
// Reporta en qué frames S/H está activo y cuántos sprites de baja prioridad hay
// ahí (candidatos a sombra). Si NINGÚN frame usa S/H, la señal no se puede
// validar con esta toma y hay que grabar un juego que sí la use.
//
//   Build:  -DAYTHER_BUILD_SPIKE=ON  →  target sh_probe
//   Run:    bin/sh_probe <recording.arp> [rom-name]
//           bin/sh_probe --live <core> <rom> [frames=15000]
//           bin/sh_probe --validate-e3 <core> <rom> [frames=15000]
//
// Modo --live (#151): en vez de replayar un .arp, corre la ROM en vivo SIN
// input (título + demos attract) N frames y reporta si algún frame activa
// S/H — para barrer una biblioteca de ROMs buscando una que ejercite la
// señal antes de grabar la toma de validación.
//
// Modo --validate-e3 (#151): el oráculo POSITIVO de la heurística E3 — con un
// juego que usa S/H (Vectorman), asigna dummies a un par de occs de la misma
// paleta con prioridades opuestas y verifica que el low-pri tinte a la MITAD
// (32 vs 64 Q2.6) bajo S/H, tinte pleno e igual sin S/H (control anti-espuria,
// exigiendo luz razonable para no validar contra un fade 0==0).
// ---------------------------------------------------------------------------
#include "ayther_session.h"
#include "ayther_recording.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#ifndef AYTHER_SOURCE_DIR
#define AYTHER_SOURCE_DIR "."
#endif

static std::string quoted(const std::string& l) {
    auto a = l.find('"'), b = l.rfind('"');
    return (a == std::string::npos || b <= a) ? std::string() : l.substr(a + 1, b - a - 1);
}
static std::string resolve(const std::string& p, const std::string& base) {
    if (p.empty() || (p.size() > 1 && p[1] == ':') || p[0] == '/' || p[0] == '\\') return p;
    return base + "/" + p;
}

// Loop compartido de conteo S/H sobre un FrameView + regs del VDP. Devuelve
// false si el core no expone los regs (sin fork).
struct ShStats {
    uint32_t sh_frames = 0, sh_with_lowpri = 0, max_lowpri = 0;
    int      first_sh = -1, last_sh = -1;
};
static bool sh_tick(ayther::AytherSession& s, const ayther::FrameView* fv,
                    uint32_t f, ShStats& st) {
    if (!fv) return true;
    size_t rsz = 0;
    const uint8_t* regs = s.vdp_regs(&rsz);
    if (!regs || rsz <= 0x0C) return false;
    if (!(regs[0x0C] & 0x08)) return true;   // reg 0x0C bit 3 = S/H enable
    ++st.sh_frames;
    if (st.first_sh < 0) st.first_sh = static_cast<int>(f);
    st.last_sh = static_cast<int>(f);
    uint32_t low = 0, high = 0;
    for (uint32_t i = 0; i < fv->sprite_occ_count; ++i)
        (fv->sprite_occs[i].priority ? high : low)++;
    if (low > 0) ++st.sh_with_lowpri;
    if (low > st.max_lowpri) st.max_lowpri = low;
    if (st.sh_frames <= 30 || (f % 60) == 0)
        std::printf("  f%-6u  S/H ON  sprites: %u baja · %u alta\n", f, low, high);
    return true;
}

static void sh_report(uint32_t nf, const ShStats& st) {
    std::printf("\n--- resumen ---\n");
    std::printf("frames totales      : %u\n", nf);
    std::printf("frames con S/H ON   : %u  (%.1f%%)\n",
                st.sh_frames, nf ? 100.0 * st.sh_frames / nf : 0.0);
    if (st.sh_frames) {
        std::printf("rango S/H           : f%d .. f%d\n", st.first_sh, st.last_sh);
        std::printf("frames S/H c/ sprite baja-pri : %u\n", st.sh_with_lowpri);
        std::printf("máx sprites baja-pri en 1 frame S/H : %u\n", st.max_lowpri);
        std::printf("\nOK — EJERCITA S/H: la señal (reg 0x0C bit3 + occ.priority) "
                    "es validable.\n");
    } else {
        std::printf("\n[SIN S/H] ningún frame activa Shadow/Highlight aquí.\n");
    }
}

// Tinte Q2.6 del sub cuyo rect matchea la occ dada (sprite_subs no lleva hash;
// se cruza por posición/tamaño — mismo patrón que hd_luma_smoke). -1 = sin sub.
static bool sub_tint_for_occ(const ayther::FrameView* fv,
                             const AytherSpriteOccurrence& oc, uint8_t out[3]) {
    if (!fv || !fv->sprite_sub_tint) return false;
    for (uint32_t s = 0; s < fv->sprite_sub_count; ++s) {
        const auto& su = fv->sprite_subs[s];
        if (su.screen_x == oc.screen_x && su.screen_y == oc.screen_y
            && su.w_tiles == oc.w_tiles && su.h_tiles == oc.h_tiles) {
            for (int c = 0; c < 3; ++c) out[c] = fv->sprite_sub_tint[s * 3 + c];
            return true;
        }
    }
    return false;
}

// Modo --validate-e3 (#151): valida la heurística E3 (S/H + prioridad baja →
// tinte ×0.5) contra el juego EN VIVO. Dos fases:
//   1. (control) frame SIN S/H con dos occs de la MISMA paleta y prioridad
//      opuesta → asigna dummies a ambas → los tintes deben ser IGUALES
//      (sin sombra espuria).
//   2. frame CON S/H, mismas condiciones → el tinte del low-pri debe ser
//      ≈ la MITAD del high-pri (misma paleta ⇒ mismo E1 base).
// El asset dummy no necesita existir: el tinte se computa en produce_frame.
static int run_validate_e3(const std::string& core, const std::string& rom,
                           uint32_t max_frames) {
    std::printf("=== sh_probe --validate-e3 ===\ncore: %s\nrom : %s\n\n",
                core.c_str(), rom.c_str());
    ayther::AytherSession::Config c;
    c.core_path = core; c.rom_path = rom; c.enable_audio = false;
    auto r = ayther::AytherSession::create(c);
    if (!r) { std::fprintf(stderr, "[FAIL] create: %s\n", r.error.message.c_str()); return 1; }
    std::unique_ptr<ayther::AytherSession>& s = *r;

    int fails = 0, done_ctrl = 0, done_sh = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("[%s] %s\n", ok ? "ok" : "FALLO", what);
        if (!ok) ++fails;
    };
    // Busca en fv dos occs de la MISMA paleta con prioridades opuestas y
    // hashes DISTINTOS (evita ambigüedad al asignar por hash).
    auto find_pair = [](const ayther::FrameView* fv, uint32_t& lo, uint32_t& hi) {
        for (uint32_t i = 0; i < fv->sprite_occ_count; ++i) {
            if (fv->sprite_occs[i].priority != 0) continue;
            for (uint32_t j = 0; j < fv->sprite_occ_count; ++j) {
                if (fv->sprite_occs[j].priority != 1) continue;
                if ((fv->sprite_occs[j].palette & 3) != (fv->sprite_occs[i].palette & 3)) continue;
                if (fv->sprite_occs[j].hash == fv->sprite_occs[i].hash) continue;
                lo = i; hi = j; return true;
            }
        }
        return false;
    };
    for (uint32_t f = 0; f < max_frames && (!done_ctrl || !done_sh); ++f) {
        const ayther::FrameView& fv = s->step();
        size_t rsz = 0;
        const uint8_t* regs = s->vdp_regs(&rsz);
        if (!regs || rsz <= 0x0C) { std::fprintf(stderr, "[FAIL] sin regs VDP\n"); return 1; }
        const bool sh = (regs[0x0C] & 0x08) != 0;
        if ((sh && done_sh) || (!sh && done_ctrl) || f < 120) continue;

        uint32_t lo = 0, hi = 0;
        if (!find_pair(&fv, lo, hi)) continue;
        const uint64_t lo_hash = fv.sprite_occs[lo].hash;
        const uint64_t hi_hash = fv.sprite_occs[hi].hash;
        s->assign_sprite(lo_hash, "e3_low_dummy.png");
        s->assign_sprite(hi_hash, "e3_high_dummy.png");
        const ayther::FrameView& fv2 = s->step();
        // El estado pudo cambiar en el frame: revalidar el par y el modo S/H.
        const uint8_t* regs2 = s->vdp_regs(&rsz);
        const bool sh2 = regs2 && (regs2[0x0C] & 0x08) != 0;
        uint32_t lo2 = 0, hi2 = 0;
        bool have = sh2 == sh && find_pair(&fv2, lo2, hi2)
                 && fv2.sprite_occs[lo2].hash == lo_hash
                 && fv2.sprite_occs[hi2].hash == hi_hash;
        uint8_t tl[3], th[3];
        if (have)
            have = sub_tint_for_occ(&fv2, fv2.sprite_occs[lo2], tl)
                && sub_tint_for_occ(&fv2, fv2.sprite_occs[hi2], th);
        s->unassign_sprite(lo_hash);
        s->unassign_sprite(hi_hash);
        if (!have) continue;

        std::printf("f%-6u %s  pal=%u  low-pri tint=%u/%u/%u  high-pri tint=%u/%u/%u\n",
                    f, sh ? "S/H ON " : "S/H off", fv2.sprite_occs[lo2].palette & 3,
                    tl[0], tl[1], tl[2], th[0], th[1], th[2]);
        if (!sh) {
            // Control: sin S/H el low-pri NO debe atenuarse (mismo E1 base).
            // Se exige luz razonable (evita el control trivial 0==0 de un fade).
            if (th[0] + th[1] + th[2] <= 90) continue;
            bool eq = true;
            for (int ch = 0; ch < 3; ++ch)
                if (std::abs(int(tl[ch]) - int(th[ch])) > 3) eq = false;
            check(eq, "sin S/H: low-pri y high-pri con el MISMO tinte (sin sombra espuria)");
            done_ctrl = 1;
        } else {
            // E3: low-pri ≈ high-pri × 0.5 por canal (Q2.6, tolerancia ±3).
            bool half = true;
            for (int ch = 0; ch < 3; ++ch)
                if (std::abs(int(tl[ch]) * 2 - int(th[ch])) > 6) half = false;
            check(half, "con S/H: el tinte del low-pri es la MITAD del high-pri (E3 x0.5)");
            check(th[0] + th[1] + th[2] > 90,
                  "con S/H: el high-pri queda a luz plena (la sombra no contamina)");
            done_sh = 1;
        }
    }
    if (!done_ctrl) { std::printf("[FALLO] sin frame de CONTROL (S/H off + par de occs)\n"); ++fails; }
    if (!done_sh)   { std::printf("[FALLO] sin frame S/H con par de occs validable\n"); ++fails; }
    std::printf("\n%s\n", fails ? "FALLOS" : "OK (0 fallos) — E3 validado contra juego real");
    return fails ? 1 : 0;
}

// Modo --live (#151): corre la ROM sin input N frames (attract) contando S/H.
static int run_live(const std::string& core, const std::string& rom, uint32_t nf) {
    std::printf("=== sh_probe --live ===\ncore: %s\nrom : %s\nframes: %u\n\n",
                core.c_str(), rom.c_str(), nf);
    ayther::AytherSession::Config c;
    c.core_path = core; c.rom_path = rom; c.enable_audio = false;
    auto r = ayther::AytherSession::create(c);
    if (!r) { std::fprintf(stderr, "[FAIL] create: %s\n", r.error.message.c_str()); return 1; }
    std::unique_ptr<ayther::AytherSession>& s = *r;
    ShStats st;
    for (uint32_t f = 0; f < nf; ++f) {
        const ayther::FrameView& fv = s->step();
        if (!sh_tick(*s, &fv, f, st)) {
            std::fprintf(stderr, "[FAIL] sin regs VDP (¿core sin fork?)\n");
            return 1;
        }
    }
    sh_report(nf, st);
    return st.sh_frames ? 0 : 3;   // exit 3 = sin S/H (para el barrido por script)
}

int main(int argc, char** argv) {
    const std::string root = AYTHER_SOURCE_DIR;
    if (argc < 2) {
        std::fprintf(stderr, "uso: sh_probe <recording.arp> [rom-name]\n"
                             "     sh_probe --live <core> <rom> [frames=15000]\n");
        return 2;
    }
    if (std::string(argv[1]) == "--live") {
        if (argc < 4) { std::fprintf(stderr, "uso: sh_probe --live <core> <rom> [frames]\n"); return 2; }
        const uint32_t nf = argc > 4 ? static_cast<uint32_t>(std::atoi(argv[4])) : 15000u;
        return run_live(argv[2], argv[3], nf);
    }
    if (std::string(argv[1]) == "--validate-e3") {
        if (argc < 4) { std::fprintf(stderr, "uso: sh_probe --validate-e3 <core> <rom> [frames]\n"); return 2; }
        const uint32_t nf = argc > 4 ? static_cast<uint32_t>(std::atoi(argv[4])) : 15000u;
        return run_validate_e3(argv[2], argv[3], nf);
    }
    const std::string arp  = argv[1];
    const std::string want = argc > 2 ? argv[2] : "aladdin";

    std::ifstream cfg(root + "/tests/test_config.toml");
    if (!cfg) { std::fprintf(stderr, "[skip] sin tests/test_config.toml\n"); return 0; }
    std::string line, core, cur_name;
    std::vector<std::pair<std::string, std::string>> roms;
    bool in_rom = false;
    while (std::getline(cfg, line)) {
        if (line.find("[[rom]]") != std::string::npos) { in_rom = true; cur_name.clear(); continue; }
        if (!in_rom && line.find("core") != std::string::npos &&
            line.find('=') != std::string::npos && core.empty())
            core = quoted(line);
        if (in_rom && line.find("name") != std::string::npos && line.find('=') != std::string::npos)
            cur_name = quoted(line);
        if (in_rom && line.find("path") != std::string::npos && line.find('=') != std::string::npos)
            roms.emplace_back(cur_name, quoted(line));
    }
    std::string rom;
    for (const auto& [n, p] : roms) if (n == want) { rom = p; break; }
    if (rom.empty() && !roms.empty()) rom = roms.front().second;
    core = resolve(core, root); rom = resolve(rom, root);
    if (core.empty() || rom.empty()) { std::fprintf(stderr, "[FAIL] config incompleta\n"); return 1; }

    auto rr = ayther::AytherRecording::load(arp);
    if (!rr) { std::fprintf(stderr, "[FAIL] no se pudo cargar %s\n", arp.c_str()); return 1; }
    const ayther::AytherRecording& rec = *rr;

    std::printf("=== sh_probe ===\ncore: %s\nrom : %s\narp : %s  (%u frames)\n\n",
                core.c_str(), rom.c_str(), arp.c_str(), rec.frame_count());

    ayther::AytherSession::Config c;
    c.core_path = core; c.rom_path = rom; c.enable_audio = false;
    auto r = ayther::AytherSession::create(c);
    if (!r) { std::fprintf(stderr, "[FAIL] create: %s\n", r.error.message.c_str()); return 1; }
    std::unique_ptr<ayther::AytherSession>& s = *r;

    const uint32_t nf = rec.frame_count();
    ShStats st;
    for (uint32_t f = 0; f < nf; ++f) {
        const ayther::FrameView* fv = s->replay_seek(rec, f);
        if (!sh_tick(*s, fv, f, st)) {
            std::fprintf(stderr, "[FAIL] sin regs VDP (¿core sin fork?)\n");
            return 1;
        }
    }
    sh_report(nf, st);
    return 0;
}
