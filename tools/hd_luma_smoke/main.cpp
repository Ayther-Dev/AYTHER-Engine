// ---------------------------------------------------------------------------
// hd_luma_smoke — E1: verifica que FrameView.sprite_sub_luma llegue CORRECTO
// por-sub cuando hay una sustitución HD activa (la pieza que el indicador del
// Lab no cubre: éste calcula la luminancia de la paleta directo, no vía el
// factor por-sub del engine). Registra un override para un hash del Genio,
// replaya la toma y comprueba que el factor de fundido de SU sub cae en un
// frame de fundido y está pleno en un frame luminoso.
//
//   Build:  -DAYTHER_BUILD_SPIKE=ON  →  target hd_luma_smoke
//   Run:    bin/hd_luma_smoke <recording.arp> [rom-name]
// ---------------------------------------------------------------------------
#include "ayther_session.h"
#include "ayther_recording.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#ifndef AYTHER_SOURCE_DIR
#define AYTHER_SOURCE_DIR "."
#endif

// Un hash del Genio (pose 0x01 "Genio Arbitro - Inicial", paleta 3) — presente
// en el SAT durante el fade-in (frame 67, negro) y ya pleno hacia el frame 80+.
static constexpr uint64_t kGenioHash = 0xa5332faadb3c7232ull;

static std::string quoted(const std::string& l) {
    auto a = l.find('"'), b = l.rfind('"');
    return (a == std::string::npos || b <= a) ? std::string() : l.substr(a + 1, b - a - 1);
}
static std::string resolve(const std::string& p, const std::string& base) {
    if (p.empty() || (p.size() > 1 && p[1] == ':') || p[0] == '/' || p[0] == '\\') return p;
    return base + "/" + p;
}

// Factor de fundido del sub cuyo screen rect matchea un hash presente en el
// frame. Devuelve -1 si no hay sub para ese hash. Como sprite_subs no lleva el
// hash, se cruza por posición/tamaño contra la occurrence del hash pedido.
// E1 pasó a tinte RGB (Q2.6): se deriva el escalar Rec.601 equivalente 0-255
// (un fundido a negro es uniforme → mismo número que el luma clásico).
static int sub_luma_for_hash(const ayther::FrameView* fv, uint64_t hash) {
    if (!fv || !fv->sprite_sub_tint) return -1;
    for (uint32_t o = 0; o < fv->sprite_occ_count; ++o) {
        const auto& oc = fv->sprite_occs[o];
        if (oc.hash != hash) continue;
        for (uint32_t s = 0; s < fv->sprite_sub_count; ++s) {
            const auto& su = fv->sprite_subs[s];
            if (su.screen_x == oc.screen_x && su.screen_y == oc.screen_y
                && su.w_tiles == oc.w_tiles && su.h_tiles == oc.h_tiles) {
                const double l = (0.299 * fv->sprite_sub_tint[s * 3 + 0]
                                + 0.587 * fv->sprite_sub_tint[s * 3 + 1]
                                + 0.114 * fv->sprite_sub_tint[s * 3 + 2])
                               * 255.0 / 64.0;
                return l > 255.0 ? 255 : static_cast<int>(l + 0.5);
            }
        }
    }
    return -1;
}

int main(int argc, char** argv) {
    const std::string root = AYTHER_SOURCE_DIR;
    if (argc < 2) { std::fprintf(stderr, "uso: hd_luma_smoke <recording.arp> [rom]\n"); return 2; }
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
            line.find('=') != std::string::npos && core.empty()) core = quoted(line);
        if (in_rom && line.find("name") != std::string::npos && line.find('=') != std::string::npos)
            cur_name = quoted(line);
        if (in_rom && line.find("path") != std::string::npos && line.find('=') != std::string::npos)
            roms.emplace_back(cur_name, quoted(line));
    }
    std::string rom;
    for (const auto& [n, p] : roms) if (n == want) { rom = p; break; }
    if (rom.empty() && !roms.empty()) rom = roms.front().second;
    core = resolve(core, root); rom = resolve(rom, root);

    auto rr = ayther::AytherRecording::load(arp);
    if (!rr) { std::fprintf(stderr, "[FAIL] no se pudo cargar %s\n", arp.c_str()); return 1; }
    const ayther::AytherRecording& rec = *rr;

    ayther::AytherSession::Config c;
    c.core_path = core; c.rom_path = rom; c.enable_audio = false;
    auto r = ayther::AytherSession::create(c);
    if (!r) { std::fprintf(stderr, "[FAIL] create: %s\n", r.error.message.c_str()); return 1; }
    std::unique_ptr<ayther::AytherSession>& s = *r;

    // Override HD del hash del Genio (el asset no necesita existir: sprite_sub_luma
    // se calcula en el resolve, independiente de si la textura carga).
    // #345: acá se encendía el compose por supresión; el sub-luma que mide esta
    // sonda sale del RESOLVE, que no dependía de esa maquinaria.
    s->assign_sprite(kGenioHash, "hd_genio_dummy.png");

    std::printf("=== hd_luma_smoke ===\narp: %s  (%u frames)\nhash: 0x%016llx\n\n",
                arp.c_str(), rec.frame_count(), static_cast<unsigned long long>(kGenioHash));

    // Buscar un frame LUMINOSO (Genio presente y pleno) y uno de FUNDIDO. Se
    // barre construyendo el peak-hold; se registra el sub-luma del hash en cada.
    // El spike ya ubicó: fundido en ~f67 (negro), pleno en ~f80+.
    int luma_dark = -1, dark_f = -1, luma_full = -1, full_f = -1;
    const uint32_t nf = rec.frame_count();
    for (uint32_t f = 0; f < nf && f < 200; ++f) {
        const ayther::FrameView* fv = s->replay_seek(rec, f);
        const int L = sub_luma_for_hash(fv, kGenioHash);
        if (L < 0) continue;
        if (L >= 240 && full_f < 0) { luma_full = L; full_f = static_cast<int>(f); }
        // el más oscuro visto con sub presente
        if (dark_f < 0 || L < luma_dark) { luma_dark = L; dark_f = static_cast<int>(f); }
    }

    std::printf("frame más OSCURO con sub del Genio: f%d  luma=%d (%.0f%% fundido)\n",
                dark_f, luma_dark, dark_f >= 0 ? (255 - luma_dark) / 2.55 : 0.0);
    std::printf("frame PLENO con sub del Genio:       f%d  luma=%d\n\n", full_f, luma_full);

    // Aserciones: existe un sub con factor CASI PLENO (fade terminado) y otro
    // MUY oscuro (mitad del fade o negro). El peak-hold debe dar 255 en pleno.
    bool ok = true;
    if (full_f < 0 || luma_full < 240) {
        std::fprintf(stderr, "[FAIL] no se vio el Genio a brillo pleno (luma>=240)\n");
        ok = false;
    }
    if (dark_f < 0 || luma_dark > 160) {
        std::fprintf(stderr, "[FAIL] no se vio el Genio fundido (luma<=160); min=%d\n", luma_dark);
        ok = false;
    }
    if (ok)
        std::printf("OK — sprite_sub_luma refleja el fundido: pleno=%d oscuro=%d.\n",
                    luma_full, luma_dark);
    return ok ? 0 : 1;
}
