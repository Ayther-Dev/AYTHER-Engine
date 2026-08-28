// ---------------------------------------------------------------------------
// recompose_spike (#270) — mide cuánto diverge una recomposición de la
// pantalla desde el estado FINAL del VDP contra el framebuffer real del
// emulador, frame a frame de una toma. No escribe render: MIDE.
//
// El motor ve el estado del VDP a fin de frame; la pantalla real se dibujó
// línea a línea con el estado vigente en cada línea (scroll/CRAM/regs a media
// pantalla). El fork expone ayther_recompose_frame(): re-renderiza el frame
// con SU MISMO renderer pero con el estado congelado a fin de frame → la
// diferencia contra fv->fb_pixels es EXACTAMENTE el costo del single-state,
// sin ruido de una reimplementación.
//
// Pases (uno por comportamiento del VDP que el render propio podría no
// modelar; el delta contra el pase base = la evidencia que pide #270):
//   base        estado final, todo modelado (el techo de la recomposición)
//   no_sh       sin shadow/highlight
//   no_limite   sin límite de sprites por línea (sin parpadeo por overflow)
//   no_mascara  sin máscara de sprites (x=0)
//   sin_window  sin plano Window (A ocupa toda la línea)
//   hs_plano    hscroll de la línea 0 para todas (sin scroll raster H)
//   vs_plano    vscroll de la columna 0 (sin vscroll por columna)
//   naive_todo  todos los anteriores a la vez (el peor render posible)
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target recompose_spike
//   Args:  <rec.arp> [f0 f1] [--stride N] [--worst N] [--dump DIR]
//   Env:   AYTHER_PROBE_ROM = ROM del juego (el core sale de test_config.toml)
//
// Con --dump escribe: un tríptico PNG (emulador | recomposición | diff) de los
// N peores frames del pase base, y base.csv con la serie por frame.
// ---------------------------------------------------------------------------
#include "ayther_env.h"
#include "ayther_session.h"
#include "ayther_recording.h"
#include <stb_image_write.h>   // impl linkeada desde el engine (tile_tex_cache)

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef AYTHER_SOURCE_DIR
#define AYTHER_SOURCE_DIR "."
#endif
using ayther::FrameView;

// Flags de ayther_recompose_frame (espejo de vdp_render.h del fork).
static constexpr unsigned kNoSh       = 0x01u;
static constexpr unsigned kNoSprLimit = 0x02u;
static constexpr unsigned kNoSprMask  = 0x04u;
static constexpr unsigned kNoWindow   = 0x08u;
static constexpr unsigned kFlatHs     = 0x10u;
static constexpr unsigned kFlatVs     = 0x20u;
using RecomposeFn = int (*)(uint16_t*, int, unsigned, int*, int*);

static std::string toml_quoted(const std::string& l) {
    const auto a = l.find('"'), b = l.rfind('"');
    return (a == std::string::npos || b <= a) ? std::string() : l.substr(a + 1, b - a - 1);
}
static std::string resolve(const std::string& p, const std::string& base) {
    if (p.empty() || (p.size() > 1 && p[1] == ':') || p[0] == '/' || p[0] == '\\') return p;
    return base + "/" + p;
}

struct Pass {
    const char* name;
    unsigned    flags;
    uint64_t    px = 0, same = 0;     // acumulados sobre todos los frames
    uint32_t    frames = 0, perfect = 0;
};

// Copia el framebuffer RGB565 del frame (compactado, sin pitch).
static bool copy_fb(const FrameView* fv, std::vector<uint16_t>& out, int& w, int& h) {
    if (!fv || !fv->fb_pixels || !fv->fb_width || !fv->fb_height) return false;
    w = (int)fv->fb_width; h = (int)fv->fb_height;
    out.resize((size_t)w * h);
    const uint8_t* src = static_cast<const uint8_t*>(fv->fb_pixels);
    for (int y = 0; y < h; ++y)
        std::memcpy(&out[(size_t)y * w], src + (size_t)y * fv->fb_pitch,
                    (size_t)w * 2);
    return true;
}

static void rgb565_to_888(uint16_t p, uint8_t* rgb) {
    const unsigned r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
    rgb[0] = (uint8_t)((r << 3) | (r >> 2));
    rgb[1] = (uint8_t)((g << 2) | (g >> 4));
    rgb[2] = (uint8_t)((b << 3) | (b >> 2));
}

// Tríptico emulador | recomposición | diff (mismatch en magenta sobre el
// frame del emulador atenuado — se ve DÓNDE está el error, no cuánto vale).
static void write_triptych(const std::string& path, const std::vector<uint16_t>& emu,
                           const std::vector<uint16_t>& rec, int w, int h) {
    const int W = w * 3 + 4;   // 2 px de separador negro entre paneles
    std::vector<uint8_t> img((size_t)W * h * 3, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = (size_t)y * w + x;
            uint8_t* e = &img[((size_t)y * W + x) * 3];
            uint8_t* r = &img[((size_t)y * W + w + 2 + x) * 3];
            uint8_t* d = &img[((size_t)y * W + 2 * w + 4 + x) * 3];
            rgb565_to_888(emu[i], e);
            rgb565_to_888(rec[i], r);
            if (emu[i] == rec[i]) {
                d[0] = (uint8_t)(e[0] / 4); d[1] = (uint8_t)(e[1] / 4); d[2] = (uint8_t)(e[2] / 4);
            } else {
                d[0] = 255; d[1] = 0; d[2] = 255;
            }
        }
    stbi_write_png(path.c_str(), W, h, 3, img.data(), W * 3);
}

struct WorstFrame {
    uint32_t frame = 0;
    uint64_t mismatch = 0;
    std::vector<uint16_t> emu, rec;
    int w = 0, h = 0;
};

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "uso: recompose_spike <rec.arp> [f0 f1] [--stride N] [--worst N] [--dump DIR]\n");
        return 2;
    }
    const std::string rec_path = argv[1];
    uint32_t f0 = 0, f1 = UINT32_MAX;
    uint32_t stride = 1, worst_n = 6;
    std::string dump_dir;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "--stride")      stride = (uint32_t)std::strtoul(next(), nullptr, 10);
        else if (a == "--worst")  worst_n = (uint32_t)std::strtoul(next(), nullptr, 10);
        else if (a == "--dump")   dump_dir = next();
        else if (f0 == 0 && f1 == UINT32_MAX && a[0] != '-') {
            f0 = (uint32_t)std::strtoul(a.c_str(), nullptr, 10);
            f1 = (uint32_t)std::strtoul(next(), nullptr, 10);
        }
    }
    if (!stride) stride = 1;

    const std::string root = AYTHER_SOURCE_DIR;
    std::string core, rom, line;
    {
        std::ifstream cfg(root + "/tests/test_config.toml");
        while (std::getline(cfg, line))
            if (line.find("core") != std::string::npos &&
                line.find('=') != std::string::npos && core.empty())
                core = toml_quoted(line);
    }
    core = resolve(core, root);
    if (const char* er = ayther::env_get("AYTHER_PROBE_ROM")) rom = er;
    if (rom.empty()) { std::fprintf(stderr, "[FAIL] falta AYTHER_PROBE_ROM\n"); return 2; }

    ayther::AytherSession::Config c;
    c.core_path = core; c.rom_path = rom; c.enable_audio = false;
    auto r = ayther::AytherSession::create(c);
    if (!r) { std::fprintf(stderr, "[FAIL] create: %s\n", r.error.message.c_str()); return 1; }
    std::unique_ptr<ayther::AytherSession>& s = *r;

    auto rc = reinterpret_cast<RecomposeFn>(s->core_export("ayther_recompose_frame"));
    if (!rc) {
        std::fprintf(stderr, "[FAIL] el core no exporta ayther_recompose_frame "
                             "(¿DLL vieja en third_party/cores?)\n");
        return 1;
    }

    auto rec = ayther::AytherRecording::load(rec_path);
    if (!rec) { std::fprintf(stderr, "[FAIL] no se pudo cargar %s\n", rec_path.c_str()); return 1; }
    if (f1 >= rec->frame_count()) f1 = rec->frame_count() ? rec->frame_count() - 1 : 0;
    if (f0 > f1) { std::fprintf(stderr, "[FAIL] rango vacío\n"); return 2; }

    Pass passes[] = {
        { "base",       0 },
        { "no_sh",      kNoSh },
        { "no_limite",  kNoSprLimit },
        { "no_mascara", kNoSprMask },
        { "sin_window", kNoWindow },
        { "hs_plano",   kFlatHs },
        { "vs_plano",   kFlatVs },
        { "naive_todo", kNoSh | kNoSprLimit | kNoSprMask | kNoWindow | kFlatHs | kFlatVs },
    };
    constexpr size_t kPasses = sizeof(passes) / sizeof(passes[0]);

    std::printf("=== recompose_spike (#270) ===\nrec  : %s (%u frames)\nrango: %u..%u stride %u\n\n",
                rec_path.c_str(), rec->frame_count(), f0, f1, stride);

    std::vector<uint16_t> emu, buf(320 * 240);
    std::vector<uint64_t> line_hist;            // mismatches del pase base por scanline
    std::vector<WorstFrame> worst;
    std::ofstream csv;
    if (!dump_dir.empty()) {
        std::filesystem::create_directories(dump_dir);
        csv.open(dump_dir + "/base.csv");
        csv << "frame,w,h,mismatch,match_pct,lines_hit,max_line,max_line_px\n";
    }
    // Histograma del match % del pase base (décimas de punto de 99 a 100 y
    // buckets gruesos abajo) → distribución del error para el reporte.
    uint32_t dist[6] = {0};   // ==100, [99.9,100), [99,99.9), [95,99), [80,95), <80
    uint32_t skipped = 0, frames_done = 0;
    bool modes_printed = false;

    for (uint32_t f = f0; f <= f1; f += stride) {
        const FrameView* fv = s->replay_seek(*rec, f);
        int w = 0, h = 0;
        if (!copy_fb(fv, emu, w, h)) { ++skipped; continue; }
        if (!modes_printed) {
            size_t rsz = 0;
            if (const uint8_t* vr = s->vdp_regs(&rsz); vr && rsz >= 0x20) {
                static const char* kHs[4] = {"full", "inv", "celda", "linea"};
                std::printf("modos VDP (f%u): %s, hscroll=%s, vscroll=%s, "
                            "shadow/highlight=%s, window reg17=%02X reg18=%02X\n\n",
                            f, (vr[12] & 1) ? "H40" : "H32", kHs[vr[11] & 3],
                            (vr[11] & 4) ? "por-columna" : "full",
                            (vr[12] & 8) ? "ON" : "off", vr[17], vr[18]);
            }
            modes_printed = true;
        }

        bool base_ok = false;
        uint64_t base_mm = 0;
        for (size_t p = 0; p < kPasses; ++p) {
            int rw = 0, rh = 0;
            if (!rc(buf.data(), (int)buf.size(), passes[p].flags, &rw, &rh) ||
                rw != w || rh != h) {
                if (p == 0) break;   // sin base no hay frame
                continue;
            }
            uint64_t same = 0;
            if (p == 0) {
                // base: además, mismatches por scanline (concentración del error)
                if ((int)line_hist.size() < h) line_hist.resize(h, 0);
                uint64_t max_line = 0; int max_line_y = 0; uint32_t lines_hit = 0;
                for (int y = 0; y < h; ++y) {
                    uint64_t lm = 0;
                    const uint16_t* e = &emu[(size_t)y * w];
                    const uint16_t* q = &buf[(size_t)y * w];
                    for (int x = 0; x < w; ++x) lm += (e[x] != q[x]);
                    line_hist[y] += lm;
                    if (lm) ++lines_hit;
                    if (lm > max_line) { max_line = lm; max_line_y = y; }
                    same += (uint64_t)w - lm;
                }
                base_ok = true;
                base_mm = (uint64_t)w * h - same;
                const double pct = 100.0 * (double)same / ((double)w * h);
                dist[pct >= 100.0 ? 0 : pct >= 99.9 ? 1 : pct >= 99.0 ? 2 :
                     pct >= 95.0 ? 3 : pct >= 80.0 ? 4 : 5]++;
                if (csv.is_open())
                    csv << f << ',' << w << ',' << h << ',' << base_mm << ','
                        << pct << ',' << lines_hit << ',' << max_line_y << ','
                        << max_line << '\n';
                // top-N peores para el dump
                if (base_mm && worst_n) {
                    const bool full = worst.size() >= worst_n;
                    if (!full || base_mm > worst.back().mismatch) {
                        WorstFrame wf;
                        wf.frame = f; wf.mismatch = base_mm; wf.w = w; wf.h = h;
                        wf.emu = emu;
                        wf.rec.assign(buf.begin(), buf.begin() + (size_t)w * h);
                        if (full) worst.pop_back();
                        worst.push_back(std::move(wf));
                        std::sort(worst.begin(), worst.end(),
                                  [](const WorstFrame& a, const WorstFrame& b) {
                                      return a.mismatch > b.mismatch;
                                  });
                    }
                }
            } else {
                const uint16_t* q = buf.data();
                for (size_t i = 0; i < (size_t)w * h; ++i) same += (emu[i] == q[i]);
            }
            passes[p].px   += (uint64_t)w * h;
            passes[p].same += same;
            passes[p].frames++;
            if (same == (uint64_t)w * h) passes[p].perfect++;
        }
        if (base_ok) ++frames_done; else ++skipped;
    }

    if (!frames_done) { std::fprintf(stderr, "[FAIL] ningún frame comparado\n"); return 1; }

    std::printf("-- pases (%% de píxeles idénticos al framebuffer del emulador) --\n");
    for (const Pass& p : passes) {
        if (!p.frames) continue;
        std::printf("  %-10s %9.4f%%   frames perfectos %5u/%u\n", p.name,
                    100.0 * (double)p.same / (double)p.px, p.perfect, p.frames);
    }

    std::printf("\n-- distribución del pase base (por frame) --\n");
    static const char* kDistName[6] = {
        "100% exacto", ">=99.9%", ">=99%", ">=95%", ">=80%", "<80%" };
    for (int i = 0; i < 6; ++i)
        std::printf("  %-12s %5u frames\n", kDistName[i], dist[i]);

    // Hotspots por scanline: dónde se CONCENTRA el error del pase base.
    uint64_t tot_mm = 0;
    for (uint64_t v : line_hist) tot_mm += v;
    if (tot_mm) {
        std::vector<int> order(line_hist.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = (int)i;
        std::sort(order.begin(), order.end(),
                  [&](int a, int b) { return line_hist[a] > line_hist[b]; });
        std::printf("\n-- scanlines con más error acumulado (base; total %llu px) --\n",
                    (unsigned long long)tot_mm);
        for (int i = 0; i < 12 && i < (int)order.size() && line_hist[order[i]]; ++i)
            std::printf("  y=%3d  %8llu px (%4.1f%%)\n", order[i],
                        (unsigned long long)line_hist[order[i]],
                        100.0 * (double)line_hist[order[i]] / (double)tot_mm);
    } else {
        std::printf("\n[OK] el pase base es PIXEL-EXACTO en todo el rango\n");
    }
    if (skipped) std::printf("\n(frames salteados: %u — fb nulo o recompose no aplica)\n", skipped);

    if (!dump_dir.empty()) {
        for (const WorstFrame& wf : worst) {
            char name[64];
            std::snprintf(name, sizeof(name), "/f%05u_mm%llu.png", wf.frame,
                          (unsigned long long)wf.mismatch);
            write_triptych(dump_dir + name, wf.emu, wf.rec, wf.w, wf.h);
        }
        std::printf("dump: %s (%zu tripticos + base.csv)\n", dump_dir.c_str(), worst.size());
    }
    return 0;
}
