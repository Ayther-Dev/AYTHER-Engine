// ---------------------------------------------------------------------------
// background_spike — validates the Fondos motor: stitch a scrolling plane into a
// full level strip, against a real ROM (componentes-plan §3.3/§3.4).
//
// No single frame holds a whole level: the plane nametable is small and wraps as
// the camera scrolls. `BackgroundStitcher` (core/src/background.rs) accumulates
// the visible cells over a take, keyed by their **level-space** position, to
// reconstruct the strip an artist repaints in HD. Turning an on-screen cell into
// a level position needs the *absolute* camera — but the VDP Hscroll only gives
// it modulo the plane. `ScrollUnwrapper` recovers the absolute camera from the
// wrapped scroll alone (game-agnostic). This harness proves both against Sonic 2:
//
//   1. warm up to gameplay, then hold RIGHT and, each frame:
//      - read the plane scroll from the VDP (Hscroll in VRAM + VSRAM), unwrap it
//        → absolute camera (Rust ScrollUnwrapper via FFI).
//      - read the plane A/B nametables from VRAM, map each visible cell to its
//        level-tile position, and feed the stitcher (Rust, via FFI).
//   2. cross-check the unwrapped camera == Sonic's absolute Camera_X_pos (RAM) →
//      the game-agnostic unwrap is correct.
//   3. the reconstructed plane-A strip is far wider than one screen, and its
//      conflict ratio is low (a coherent, mostly-static level) → stitching works.
//
// Headless, no SDL/Vulkan (like determinism_spike); links ayther_core.
//
//   background_spike <core_vram.dll> <rom> [--warmup N] [--frames N] [--verbose]
// ---------------------------------------------------------------------------

#include "libretro_host/retro_runner.h"
#include "parallax_bands.h"   // #231 EM-8.0: la columna de nivel por banda

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ---- Fondos FFI (background.rs) --------------------------------------------
struct AytherBgStitcher;
struct AytherScrollUnwrapper;
extern "C" AytherBgStitcher* ayther_bg_stitcher_new();
extern "C" void   ayther_bg_stitcher_free(AytherBgStitcher*);
extern "C" void   ayther_bg_stitcher_observe(AytherBgStitcher*, uint8_t plane, int32_t lx, int32_t ly, uint32_t cell);
extern "C" size_t ayther_bg_stitcher_cell_count(const AytherBgStitcher*, uint8_t plane);
extern "C" size_t ayther_bg_stitcher_conflicts(const AytherBgStitcher*, uint8_t plane);
extern "C" size_t ayther_bg_stitcher_animated_cells(const AytherBgStitcher*, uint8_t plane);
extern "C" bool   ayther_bg_stitcher_bounds(const AytherBgStitcher*, uint8_t plane, int32_t* out4);
extern "C" AytherScrollUnwrapper* ayther_scroll_unwrapper_new(int32_t period);
extern "C" void  ayther_scroll_unwrapper_free(AytherScrollUnwrapper*);
extern "C" int64_t ayther_scroll_unwrapper_push(AytherScrollUnwrapper*, int32_t wrapped);

static constexpr uint16_t BTN_START = 1u << 3;
static constexpr uint16_t BTN_RIGHT = 1u << 7;

// Sonic 2: Sonic X/Y (spawn detector) + absolute camera X (ground truth).
static constexpr uint32_t OFF_SONIC_X  = 0xB008;
static constexpr uint32_t OFF_SONIC_Y  = 0xB00C;
static constexpr uint32_t OFF_CAMERA_X = 0xEE00;

// 68k word read from the word-swapped work RAM (LE at even offset = BE word).
static int be16_bus(const uint8_t* ram, size_t sz, uint32_t off) {
    if (off + 1 >= sz) return 0;
    return static_cast<int16_t>(ram[off] | (ram[off + 1] << 8));
}
// VRAM/VSRAM words are little-endian in the fork buffer.
static uint32_t vrd16(const uint8_t* v, size_t sz, uint32_t o) {
    return (o + 1 < sz) ? (uint32_t)v[o] | ((uint32_t)v[o + 1] << 8) : 0u;
}
static uint32_t vrd32(const uint8_t* v, size_t sz, uint32_t o) {
    return (o + 3 < sz) ? (uint32_t)v[o] | ((uint32_t)v[o + 1] << 8)
                        | ((uint32_t)v[o + 2] << 16) | ((uint32_t)v[o + 3] << 24) : 0u;
}
static uint32_t vsrd32(const uint8_t* vs, size_t sz, uint32_t col) {
    const uint32_t o = col * 4u;
    return (o + 3 < sz) ? (uint32_t)vs[o] | ((uint32_t)vs[o + 1] << 8)
                        | ((uint32_t)vs[o + 2] << 16) | ((uint32_t)vs[o + 3] << 24) : 0u;
}
static int imod(int a, int m) { return m > 0 ? ((a % m) + m) % m : a; }

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
            "background_spike — validate the Fondos stitcher + scroll unwrap vs a real ROM\n"
            "usage: %s <core_vram.dll> <rom> [--warmup N] [--frames N] [--verbose]\n", argv[0]);
        return 2;
    }
    const std::string core = argv[1], rom = argv[2];
    int warmup = 1200, frames = 320; bool verbose = false;
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--warmup") && i + 1 < argc) warmup = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--verbose")) verbose = true;
    }

    RetroRunner runner;
    if (!runner.init(core, rom)) { std::fprintf(stderr, "[fondos] init failed\n"); return 1; }
    runner.subscribe_all_supported();   // sin esto el fork no observa nada
    std::fprintf(stdout, "[fondos] core fps=%.4f pixfmt=%u\n", runner.fps(), runner.pixel_format());
    const uint8_t* ram = runner.work_ram();
    const size_t   ramsz = runner.work_ram_size();

    // Warm-up: reach gameplay (tap START until the player spawns).
    int reached = -1;
    for (int i = 0; i < warmup; ++i) {
        runner.set_input(0, (i % 32 == 0) ? BTN_START : 0);
        runner.run_frame();
        if (ram && ramsz >= 0x10000 && be16_bus(ram, ramsz, OFF_SONIC_X) != 0
            && be16_bus(ram, ramsz, OFF_SONIC_Y) != 0) { reached = i; break; }
    }
    std::fprintf(stdout, "[fondos] gameplay reached at frame %d\n", reached);

    const uint8_t* vram = runner.video_ram();  const size_t vsz  = runner.video_ram_size();
    const uint8_t* regs = runner.vdp_regs();   const size_t rsz  = runner.vdp_regs_size();
    const uint8_t* vsram= runner.vsram();       const size_t vssz = runner.vsram_size();
    int fail = 0;
    auto check = [&](bool ok, const char* m) {
        std::fprintf(stdout, "  [%s] %s\n", ok ? "ok" : "FAIL", m); if (!ok) ++fail;
    };
    if (!vram || !regs || rsz < 0x20 || !vsram) {
        std::fprintf(stderr, "[fondos] core does not expose the VDP memory (need the fork). ABORT.\n");
        return 3;
    }

    // Plane geometry + hscroll table config are stable within a level.
    auto cells = [](int b) { return b == 1 ? 64 : (b == 3 ? 128 : 32); };
    const int wc = cells(regs[0x10] & 3), hc = cells((regs[0x10] >> 4) & 3);
    const int wpx = wc * 8, hpx = hc * 8;
    const bool h40 = (regs[0x0C] & 0x81) == 0x81;
    const int scr_cols = h40 ? 40 : 32, scr_rows = 28;
    std::fprintf(stdout, "[fondos] plane %dx%d cells (%dx%d px)  screen %dx%d cells\n",
                 wc, hc, wpx, hpx, scr_cols, scr_rows);

    AytherBgStitcher* st = ayther_bg_stitcher_new();
    // One unwrapper per plane axis we track. Plane A rides the camera; plane B
    // scrolls at its own (parallax) rate — each needs its own unwrap.
    AytherScrollUnwrapper* uxA = ayther_scroll_unwrapper_new(wpx);
    AytherScrollUnwrapper* uyA = ayther_scroll_unwrapper_new(hpx);
    AytherScrollUnwrapper* uxB = ayther_scroll_unwrapper_new(wpx);

    const uint32_t hscb = ((uint32_t)regs[0x0D] << 10) & 0xFC00;
    const uint32_t hmaskT[4] = { 0x00, 0x07, 0xF8, 0xFF };
    const uint32_t hmask = hmaskT[regs[0x0B] & 3];   // per-line/-cell hscroll mask
    const uint32_t baseA = (uint32_t)(regs[0x02] & 0x38) << 10;
    const uint32_t baseB = (uint32_t)(regs[0x04] & 0x07) << 13;

    // Plane B parallax is done with *per-line* Hscroll: distinct vertical bands
    // scroll at different rates. Sample plane B's scroll at a few screen lines
    // and unwrap each → measure its ratio against the foreground (plane A).
    const int  kBands = 4;
    const int  bandLine[kBands] = { 40, 100, 160, 200 };   // sky · far hills · near · water
    AytherScrollUnwrapper* uB[kBands];
    int64_t absB[kBands] = {0}, abs0B[kBands] = {0};
    for (int i = 0; i < kBands; ++i) uB[i] = ayther_scroll_unwrapper_new(wpx);

    // Pre-roll: run RIGHT to get past the near-stationary start of the level
    // (Sonic accelerating from 0). There, the level barely scrolls but decorative
    // tiles animate in place — folding that into the stitch inflates the conflict
    // count with animation rather than the scrolling behaviour we're validating.
    for (int i = 0; i < 200; ++i) { runner.set_input(0, BTN_RIGHT); runner.run_frame(); }
    const int cam_ram0 = be16_bus(ram, ramsz, OFF_CAMERA_X);
    std::fprintf(stdout, "[fondos] pre-roll done, camera at %dpx; stitching %d frames of scroll\n",
                 cam_ram0, frames);

    // #418: guardar ACÁ el estado para la sonda de tiles animados del final.
    //
    // Esa sonda corre parado, y hasta ahora corría donde hubiera terminado el
    // paneo — o sea, en un lugar distinto según `--frames`. Con 320 frames caía
    // frente a unas flores de EHZ y detectaba 48 celdas animadas; con 1200
    // terminaba en un tramo pelado y detectaba 0, y el check se iba en rojo sin
    // que hubiera nada roto. Medía dónde paró la cámara, no el clasificador.
    std::vector<uint8_t> probe_state;
    const bool have_state = runner.serialize(probe_state);
    if (!have_state)
        std::fprintf(stdout, "[fondos] aviso: sin savestate, la sonda de animados corre "
                             "donde termine el paneo (#418)\n");

    // Scroll a while, stitching each frame.
    int64_t absxA = 0, absyA = 0, absxB = 0, abs0A = 0;
    ayther::BandCameras bands[2];   // #231 EM-8.0
    for (int f = 0; f < frames; ++f) {
        runner.set_input(0, BTN_RIGHT);
        runner.run_frame();

        // Camera scroll from the VDP (line 0). Plane A = low word, B = high word.
        const uint32_t hw = vrd32(vram, vsz, hscb);
        const int camxA = imod(-(int)(hw & 0x3FF), wpx);
        const int camxB = imod(-(int)((hw >> 16) & 0x3FF), wpx);
        const uint32_t vs = vsrd32(vsram, vssz, 0);
        const int camyA = imod((int)(vs & 0x3FF), hpx);

        absxA = ayther_scroll_unwrapper_push(uxA, camxA);
        absyA = ayther_scroll_unwrapper_push(uyA, camyA);
        absxB = ayther_scroll_unwrapper_push(uxB, camxB);
        if (f == 0) abs0A = absxA;   // origin (unwrapper seeds at the wrapped value)

        // Plane B scroll per band (its own per-line Hscroll entry, high word).
        for (int i = 0; i < kBands; ++i) {
            const uint32_t hwl = vrd32(vram, vsz, hscb + (((uint32_t)bandLine[i] & hmask) << 2));
            const int camBl = imod(-(int)((hwl >> 16) & 0x3FF), wpx);
            absB[i] = ayther_scroll_unwrapper_push(uB[i], camBl);
            if (f == 0) abs0B[i] = absB[i];
        }

        // #231 EM-8.0: la columna de nivel POR BANDA — la MISMA regla que usa
        // la sesion. Cuando vivia adentro del bucle de ayther_session.cpp este
        // oraculo no la veia (llama al stitcher directo) y el cambio no se
        // podia medir.
        {
            auto rd32v = [&](uint32_t a) { return vrd32(vram, vsz, a); };
            for (uint8_t p = 0; p < 2; ++p) {
                bands[p].configure(scr_rows, wpx);
                bands[p].observe(rd32v, hscb, hmask, p);
            }
        }

        // For each plane, map the visible window to level tiles and observe.
        struct L { uint8_t plane; uint32_t base; int64_t camx, camy; };
        const L layers[2] = { { 0, baseA, absxA, absyA }, { 1, baseB, absxB, absyA } };
        for (const L& l : layers) {
            const int cam_col = (int)(l.camx / 8), cam_row = (int)(l.camy / 8);
            // Skip the rightmost few columns: the game streams the incoming
            // column into the nametable 1–2 cells before it is actually on
            // screen, so reading the very edge picks up "future" tiles that
            // collide with the correct value once that level_x is properly seen.
            for (int scy = 0; scy < scr_rows; ++scy) {
                for (int scx = 0; scx < scr_cols - 3; ++scx) {
                    const int lvx = cam_col + scx +
                        bands[l.plane].offset_from_top(scy);
                    const int lvy = cam_row + scy;
                    const int ntc = imod(lvx, wc), ntr = imod(lvy, hc);
                    const uint32_t w = vrd16(vram, vsz, l.base + (uint32_t)(ntr * wc + ntc) * 2u);
                    if ((w & 0x7FF) == 0) continue;                 // empty cell
                    ayther_bg_stitcher_observe(st, l.plane, lvx, lvy, w & 0x7FFF);
                }
            }
        }
    }

    // ---- Validate -----------------------------------------------------------
    const int cam_ram = be16_bus(ram, ramsz, OFF_CAMERA_X);   // absolute (grows through the level)
    // The unwrapper's origin is the wrapped scroll at stitch start, so it tracks
    // the camera up to a whole-plane offset (harmless: the nametable maps mod the
    // plane). Validate the game-agnostic unwrap by its DELTA over the take: the
    // distance it reports must equal the distance the RAM camera moved.
    const int64_t moved_vdp = absxA - abs0A;
    const int64_t moved_ram = (int64_t)cam_ram - cam_ram0;
    std::fprintf(stdout, "\n== Fondos: stitch + unwrap vs ROM ==\n");
    std::fprintf(stdout, "  camera moved: unwrapped VDP=%lldpx  RAM=%lldpx\n",
                 (long long)moved_vdp, (long long)moved_ram);
    const int64_t dmv = moved_vdp - moved_ram; const int64_t admv = dmv < 0 ? -dmv : dmv;
    check(admv <= 8, "unwrapped VDP scroll delta == RAM camera delta  → absolute scroll recovered");

    // ---- Plane B parallax: per-band scroll ratio vs the foreground ----------
    // Each vertical band of plane B scrolls at its own rate (per-line Hscroll).
    // ratio = bandMoved / foregroundMoved: 0 = static (sky), <1 = distant
    // parallax, ~1 = moves with the game. Genuine multi-layer parallax means the
    // bands differ and none outruns the foreground.
    std::fprintf(stdout, "\n== Plano B: parallax por banda (ratio vs plano A) ==\n");
    double rmin = 1e9, rmax = -1e9;
    for (int i = 0; i < kBands; ++i) {
        const int64_t mb = absB[i] - abs0B[i];
        const double r = moved_vdp ? (double)mb / (double)moved_vdp : 0.0;
        if (r < rmin) rmin = r; if (r > rmax) rmax = r;
        std::fprintf(stdout, "  línea %3d: movió %4lldpx  ratio %.2f\n",
                     bandLine[i], (long long)mb, r);
    }
    check(rmax <= 1.10, "ninguna banda de B supera al plano A  → capa de fondo/parallax");
    check(rmin < rmax - 0.10 || rmin < 0.90,
          "las bandas de B scrollean a ratios distintos/menores  → parallax multicapa legible");

    int bA[4] = {0,0,0,0}, bB[4] = {0,0,0,0};
    const bool okA = ayther_bg_stitcher_bounds(st, 0, bA);
    ayther_bg_stitcher_bounds(st, 1, bB);
    const size_t cA = ayther_bg_stitcher_cell_count(st, 0), fA = ayther_bg_stitcher_conflicts(st, 0);
    const size_t cB = ayther_bg_stitcher_cell_count(st, 1), fB = ayther_bg_stitcher_conflicts(st, 1);
    const int wA = okA ? bA[2] - bA[0] + 1 : 0;

    std::fprintf(stdout, "  plane A: %zu cells, strip %d tiles wide  (x %d..%d, y %d..%d), conflicts %zu\n",
                 cA, wA, bA[0], bA[2], bA[1], bA[3], fA);
    std::fprintf(stdout, "  plane B: %zu cells, x %d..%d, conflicts %zu\n", cB, bB[0], bB[2], fB);

    check(okA && cA > 0, "plane A stitched (>=1 cell)");
    check(wA > scr_cols * 2,
          "reconstructed strip far wider than one screen  → cross-frame stitch works");
    // #231 EM-8.0 — el plano B tiene que ENSANCHARSE, igual que el A.
    //
    // Este check faltaba, y su ausencia dejó pasar el bug meses: con una cámara
    // única por plano, las 45 bandas de parallax de B colapsaban en las mismas 37
    // columnas (menos de UNA pantalla) mientras el A reconstruía 607. El arte no
    // faltaba: estaba apilado en el lugar equivocado.
    //
    // Y `conflicts` no lo veía. Las bandas se distinguen por FILA, así que dos
    // bandas apiladas escriben en (col, fila) distintos y jamás chocan: el
    // contador de conflictos es estructuralmente ciego a este error. Por eso el
    // check es sobre el ANCHO, que es lo único que cambia cuando las bandas
    // dejan de pisarse.
    const int wB = bB[2] - bB[0] + 1;
    check(wB > scr_cols,
          "plano B tambien mas ancho que una pantalla  → las bandas no colapsan");
    // #231 EM-8.0, la otra mitad — la lamina es 2D, no una tira.
    //
    // El ancho solo no prueba nada de las filas: una tira 1D perfecta pasa el
    // check de arriba y sigue sin tener con que llenar el area extendida, porque
    // los huecos MEDIDOS del widescreen no son laterales — caen todos en las
    // filas de arriba, que la ventana vertical mostro en otro momento del paneo.
    // Que la lamina cubra mas filas que la pantalla es lo que dice que la fila
    // de nivel esta des-enrollada del V-scroll y no es la de nametable.
    const int hA = okA ? bA[3] - bA[1] + 1 : 0;
    std::fprintf(stdout, "  plane A: %d filas de nivel (pantalla %d)\n", hA, scr_rows);
    check(hA > scr_rows,
          "la lamina cubre mas filas que la pantalla  → acumula en 2D, no una tira");
    // Conflict ratio: cells re-seen with a different code = animated tiles (EHZ
    // has waterfalls/flowers) + residual streaming-edge reads. It is a QUALITY
    // signal, not a stitcher failure: a low ratio means most level cells land at
    // one stable position (the strip is coherent); the remainder is real
    // animation the Fondos feature will classify (§3.1 palette/orientation axes).
    const double confRatio = cA ? (double)fA / (double)cA : 1.0;
    const size_t anA = ayther_bg_stitcher_animated_cells(st, 0);
    std::fprintf(stdout, "  plane A conflict ratio = %.1f%%,  animated cells during scroll = %zu\n",
                 confRatio * 100.0, anA);
    check(confRatio < 0.20,
          "mayoría de celdas en posición estable  → tira coherente (resto = animación)");

    ayther_scroll_unwrapper_free(uxA); ayther_scroll_unwrapper_free(uyA);
    ayther_scroll_unwrapper_free(uxB);
    for (int i = 0; i < kBands; ++i) ayther_scroll_unwrapper_free(uB[i]);
    ayther_bg_stitcher_free(st);

    // ---- Animated-tile classifier: a stationary probe -----------------------
    // Standing still, the level cells don't move but decorative tiles cycle in
    // place (EHZ waterfalls / flowers). Feeding a fresh stitcher for a while,
    // keyed by screen cell, the classifier should flag those animated cells —
    // the signal the per-layer export needs to not freeze animation into a PNG.
    // #418: la sonda vuelve al MISMO lugar siempre, sea cual sea `--frames`.
    if (have_state) runner.unserialize(probe_state);
    for (int i = 0; i < 30; ++i) { runner.set_input(0, 0); runner.run_frame(); }  // settle
    AytherBgStitcher* stA = ayther_bg_stitcher_new();
    // Verdad medida ACÁ, en paralelo al clasificador: por celda de pantalla, el
    // primer código visto y si alguna vez cambió. Es casi la misma cuenta que
    // hace el stitcher, y ese es el punto — permite distinguir «el clasificador
    // está roto» de «en este lugar no hay nada que clasificar», que es
    // exactamente lo que #418 no podía distinguir.
    const size_t grid_n = (size_t)scr_rows * (size_t)scr_cols;
    std::vector<uint32_t> first(grid_n, 0);
    std::vector<uint8_t>  seen(grid_n, 0), changed(grid_n, 0);
    for (int f = 0; f < 180; ++f) {
        runner.set_input(0, 0);
        runner.run_frame();
        for (int scy = 0; scy < scr_rows; ++scy)
            for (int scx = 0; scx < scr_cols - 3; ++scx) {
                const uint32_t w = vrd16(vram, vsz, baseA + (uint32_t)(scy * wc + scx) * 2u);
                if ((w & 0x7FF) == 0) continue;
                const uint32_t code = w & 0x7FFF;
                ayther_bg_stitcher_observe(stA, 0, scx, scy, code);   // key = screen cell
                const size_t k = (size_t)scy * (size_t)scr_cols + (size_t)scx;
                if (!seen[k]) { seen[k] = 1; first[k] = code; }
                else if (first[k] != code) changed[k] = 1;
            }
    }
    size_t truth_cells = 0, truth_anim = 0;
    for (size_t k = 0; k < grid_n; ++k) { truth_cells += seen[k]; truth_anim += changed[k]; }
    const size_t stat_cells = ayther_bg_stitcher_cell_count(stA, 0);
    const size_t stat_anim  = ayther_bg_stitcher_animated_cells(stA, 0);
    ayther_bg_stitcher_free(stA);
    std::fprintf(stdout, "\n== Clasificador de tiles animados (parado) ==\n");
    std::fprintf(stdout, "  celdas de pantalla=%zu, animadas detectadas=%zu (%.1f%%)"
                         "  ·  verdad medida: %zu / %zu\n",
                 stat_cells, stat_anim, stat_cells ? 100.0 * stat_anim / stat_cells : 0.0,
                 truth_anim, truth_cells);
    // El check ya NO es «hubo animación», que dependía de dónde parara la
    // cámara: es que el clasificador coincida con lo que de verdad cambió acá.
    check(stat_cells == truth_cells && stat_anim == truth_anim,
          "el clasificador coincide con la verdad medida  → insumo del export por-capa");
    // Y la no-vacuidad se dice en voz alta en vez de fingir un rojo. Con la
    // sonda anclada esto es una propiedad estable del lugar, no una lotería:
    // si sale 0, lo que hay que mover es la posición de la sonda.
    if (truth_anim == 0)
        std::fprintf(stdout, "  [aviso] en esta posición no hay tiles animados: el check pasó "
                             "por acuerdo, no por detección (#418)\n");
    else
        check(stat_anim > 0, "…y detectó los tiles animados reales de EHZ");

    std::fprintf(stdout, "\n============================================================\n");
    if (fail == 0)
        std::fprintf(stdout,
            " OK — Fondos (stitch de plano + unwrap de scroll) VALIDADO vs ROM.\n"
            "   El eje de Fondos (export por capa + parallax scroll-aware) es factible.\n");
    else
        std::fprintf(stdout, " FALLO (%d) — revisar la salida de arriba.\n", fail);
    std::fprintf(stdout, "============================================================\n");
    (void)verbose;
    return fail ? 5 : 0;
}
