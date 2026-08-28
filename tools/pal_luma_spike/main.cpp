// ---------------------------------------------------------------------------
// pal_luma_spike — E1 (plan lab-estados-de-pose-plan.md): valida la señal de
// LUMINANCIA de una paleta a lo largo de una toma, como base para el factor de
// fundido que el reemplazo HD tiene que aplicar. Replaya el .arp, lee la CRAM
// viva por frame, calcula la luminancia media de cada paleta (Rec.601 sobre los
// colores no-transparentes) y, para la paleta objetivo, imprime el factor
// normalizado contra el peak-hold (referencia v1 del plan).
//
// El RIESGO que valida: que la luminancia de la paleta del Genio realmente
// dibuje la curva del fade-in (≈0 en el negro, 1.0 al verse plena) y que el
// peak-hold dé un factor estable y monótono — antes de tocar el render.
//
//   Build:  -DAYTHER_BUILD_SPIKE=ON  →  target pal_luma_spike
//   Run:    bin/pal_luma_spike <recording.arp> [pal 0-3] [rom-name]
//           (core+rom de tests/test_config.toml; pal por defecto 3 = Genio)
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

static std::string quoted(const std::string& l) {
    auto a = l.find('"'), b = l.rfind('"');
    return (a == std::string::npos || b <= a) ? std::string() : l.substr(a + 1, b - a - 1);
}
static std::string resolve(const std::string& p, const std::string& base) {
    if (p.empty() || (p.size() > 1 && p[1] == ':') || p[0] == '/' || p[0] == '\\') return p;
    return base + "/" + p;
}

// Luminancia media (0..1) de una línea de paleta desde la CRAM viva. color_ram()
// devuelve la CRAM EMPAQUETADA (u16 LE por color, R=bits0-2 · G=3-5 · B=6-8 —
// NO el layout Genesis "con huecos"; verificado en el render VRAM). Índice 0 =
// transparente, se ignora. Devuelve -1 si la línea no está en la CRAM.
static double palette_luma(const uint8_t* cram, size_t csz, int pal) {
    const size_t base = static_cast<size_t>(pal) * 16u * 2u;   // 16 colores × 2 bytes
    if (base + 16 * 2 > csz) return -1.0;
    double sum = 0.0; int n = 0;
    for (int i = 1; i < 16; ++i) {                              // saltear el 0 (transparente)
        const size_t ce = base + static_cast<size_t>(i) * 2u;
        const uint16_t v = static_cast<uint16_t>(cram[ce] | (cram[ce + 1] << 8));
        const double r = (v & 7) / 7.0;
        const double g = ((v >> 3) & 7) / 7.0;
        const double b = ((v >> 6) & 7) / 7.0;
        sum += 0.299 * r + 0.587 * g + 0.114 * b;              // Rec.601
        ++n;
    }
    return n ? sum / n : -1.0;
}

int main(int argc, char** argv) {
    const std::string root = AYTHER_SOURCE_DIR;
    if (argc < 2) {
        std::fprintf(stderr,
            "uso: pal_luma_spike <recording.arp> [pal 0-3] [rom-name]\n");
        return 2;
    }
    const std::string arp  = argv[1];
    const int         pal  = argc > 2 ? std::atoi(argv[2]) : 3;
    const std::string want = argc > 3 ? argv[3] : "aladdin";

    // --- config: core + rom (tests/test_config.toml, igual que los otros spikes)
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

    std::printf("=== pal_luma_spike ===\ncore: %s\nrom : %s\narp : %s  (%u frames)\npal : %d\n\n",
                core.c_str(), rom.c_str(), arp.c_str(), rec.frame_count(), pal);

    ayther::AytherSession::Config c;
    c.core_path = core; c.rom_path = rom; c.enable_audio = false;
    auto r = ayther::AytherSession::create(c);
    if (!r) { std::fprintf(stderr, "[FAIL] create: %s\n", r.error.message.c_str()); return 1; }
    std::unique_ptr<ayther::AytherSession>& s = *r;

    // --- barrido: luminancia por frame + factor normalizado (peak-hold) --------
    const uint32_t nf = rec.frame_count();
    std::vector<double> luma(nf, -1.0);
    double peak = 0.0;
    int peak_frame = -1;
    for (uint32_t f = 0; f < nf; ++f) {
        const ayther::FrameView* fv = s->replay_seek(rec, f);
        if (!fv) continue;
        size_t csz = 0;
        const uint8_t* cram = s->color_ram(&csz);
        if (!cram || csz == 0) {
            std::fprintf(stderr, "[FAIL] sin CRAM (¿core sin fork _vram?)\n");
            return 1;
        }
        const double L = palette_luma(cram, csz, pal);
        luma[f] = L;
        if (L > peak) { peak = L; peak_frame = static_cast<int>(f); }
    }
    if (peak <= 0.0) { std::fprintf(stderr, "[FAIL] paleta %d nunca tuvo luz\n", pal); return 1; }

    // ASCII de la curva + el factor. Sub-muestreo cada `step` para no inundar.
    std::printf("peak luma = %.3f @ frame %d\n\n", peak, peak_frame);
    std::printf("frame | luma  | factor | barra\n");
    std::printf("------+-------+--------+----------------------------------------\n");
    const uint32_t step = nf > 240 ? nf / 240 : 1;   // ~240 filas máx
    for (uint32_t f = 0; f < nf; f += step) {
        const double L = luma[f] < 0 ? 0.0 : luma[f];
        const double factor = L / peak;              // referencia peak-hold
        const int bars = static_cast<int>(factor * 40.0 + 0.5);
        std::string bar(bars, '#');
        std::printf("%5u | %.3f | %.3f  | %s\n", f, L, factor, bar.c_str());
    }

    // --- resumen: transiciones de fundido (cruces de 0.1 y 0.9) ----------------
    std::printf("\n--- transiciones de fundido (factor cruza 0.1 / 0.9) ---\n");
    double prev = luma[0] < 0 ? 0.0 : luma[0] / peak;
    for (uint32_t f = 1; f < nf; ++f) {
        const double cur = luma[f] < 0 ? 0.0 : luma[f] / peak;
        auto cross = [&](double th, const char* dir_up, const char* dir_dn) {
            if (prev < th && cur >= th) std::printf("  f%-5u  %s (factor %.2f→%.2f)\n", f, dir_up, prev, cur);
            if (prev >= th && cur < th) std::printf("  f%-5u  %s (factor %.2f→%.2f)\n", f, dir_dn, prev, cur);
        };
        cross(0.1, "sube  0.1 (empieza a verse)", "baja  0.1 (casi negro)");
        cross(0.9, "sube  0.9 (plena)",          "baja  0.9 (empieza a fundir)");
        prev = cur;
    }

    std::printf("\nOK — la paleta %d tiene curva de luminancia usable (peak %.3f).\n", pal, peak);
    return 0;
}
