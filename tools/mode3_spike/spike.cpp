// ---------------------------------------------------------------------------
// mode3_spike — validates risk #2 (VDP reads) and Mode 3 against a real ROM.
//
// The Components engine's "Actor / Mode 3" disambiguates identical on-screen
// entities by reading each one's *world position* from game RAM and converting
// it to a *screen position* using the camera scroll read from the VDP:
//
//     screen ≈ world − camera_scroll + pivot   (ram_anchor::assign_sprites)
//
// Two things were UNVALIDATED without a runtime + ROM (estado-motor-componentes
// §3.4/§3.5):
//   (#2) reading the camera scroll from the VDP — Hscroll table (in VRAM) +
//        VSRAM — via the fork's private memory ids (video_ram / vdp_regs 0x101
//        / vsram 0x107).  Blocks Mode 3's scroll AND the whole Fondos axis.
//   (M3) that feeding real world_pos + real VDP scroll + real SAT positions to
//        assign_sprites lands the player on its actual hardware sprites.
//
// This headless harness (no SDL/Vulkan, like determinism_spike) proves both:
//   1. load core + ROM, warm up to gameplay (tap START, then hold RIGHT so the
//      camera scrolls — a big scroll makes the sign/offset test sensitive).
//   2. read the camera scroll from the VDP (Hscroll from VRAM + VSRAM) and,
//      independently, from Sonic 2's camera RAM → cross-check they agree mod
//      the plane size  →  confirms the VDP read (risk #2).
//   3. read the player world_pos from work RAM, parse this frame's SAT, run
//      ram_anchor::assign_sprites  →  confirm the player's sprites land on the
//      player entity (Mode 3).
//
// BYOR: you provide the ROM + a VRAM-exposing fork core.
//
//   mode3_spike <core.dll> <rom> [--warmup N] [--settle N] [--verbose]
//
// Sonic 2 (World) RAM map — 68000 work RAM offsets (big-endian words):
//   0xB008 Sonic X    0xB00C Sonic Y       (Sonic object @ 0xFFB000)
//   0xB048 Tails X    0xB04C Tails Y       (Tails object @ 0xFFB040)
//   0xEE00 camera X   0xEE04 camera Y      (Camera_X_pos @ 0xFFEE00)
// ---------------------------------------------------------------------------

#include "libretro_host/retro_runner.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// The one new Rust entry point (kept out of the cxx bridge — see lib.rs).
// Fills out_assign[j] with the entity index claiming sprite j, or -1.
extern "C" size_t ayther_mode3_assign_sprites(
    const uint64_t* ent_ids, const int32_t* ent_x, const int32_t* ent_y, size_t n_ent,
    int32_t scroll_x, int32_t scroll_y,
    int32_t pivot_x, int32_t pivot_y, int32_t half_w, int32_t half_h,
    const int16_t* spr_x, const int16_t* spr_y, size_t n_spr,
    int32_t* out_assign);

// Game-profile FFI (game_profile.rs): read entities from RAM by a TOML profile
// and assign this frame's SAT sprites to them. This is the Mode 3 input
// plumbing — the spike drives it end-to-end instead of hardcoding offsets.
struct AytherGameProfile;
extern "C" AytherGameProfile* ayther_game_profile_load(const char* path);
extern "C" void   ayther_game_profile_free(AytherGameProfile*);
extern "C" size_t ayther_game_profile_entities(
    const AytherGameProfile*, const uint8_t* ram, size_t ramsz,
    uint64_t* out_id, int32_t* out_wx, int32_t* out_wy, size_t cap);
extern "C" size_t ayther_game_profile_assign(
    const AytherGameProfile*, const uint8_t* ram, size_t ramsz,
    int32_t scroll_x, int32_t scroll_y, int32_t plane_w, int32_t plane_h,
    const int16_t* spr_x, const int16_t* spr_y, size_t n_spr, uint64_t* out_ent_id);

// Friendly name for a known Sonic 2 object id (display only).
static const char* ent_name(uint64_t id) {
    return id == 0xB000 ? "Sonic" : id == 0xB040 ? "Tails" : "obj";
}

// Libretro joypad button ids → bit positions.
static constexpr uint16_t BTN_START = 1u << 3;
static constexpr uint16_t BTN_RIGHT = 1u << 7;

// Sonic 2 work-RAM offsets. Both playable characters share the same object
// layout (X at +0x08, Y at +0x0C): Sonic @ 0xFFB000, Tails @ 0xFFB040 — two
// near-identical entities on screen, the exact case Mode 3 disambiguates.
static constexpr uint32_t OFF_SONIC_X  = 0xB008;
static constexpr uint32_t OFF_SONIC_Y  = 0xB00C;
static constexpr uint32_t OFF_TAILS_X  = 0xB048;
static constexpr uint32_t OFF_TAILS_Y  = 0xB04C;
static constexpr uint32_t OFF_CAMERA_X = 0xEE00;
static constexpr uint32_t OFF_CAMERA_Y = 0xEE04;

// ---- RAM reads -------------------------------------------------------------
// The fork's work RAM (retro_get_memory_data(0)) is word-swapped on little-
// endian hosts: the byte at logical 68k address `off` lives at `off^1`. For a
// 16-bit value at an even offset, a little-endian read of the two consecutive
// raw bytes therefore reconstructs the true big-endian 68k word (raw[off]=low,
// raw[off+1]=high). This mirrors AytherSession::ram_u16 (the canonical view);
// ayther_sonic_read_xy reads big-endian *directly* and so is NOT valid on the
// raw runner buffer — we do the swap here.
static int be16_bus(const uint8_t* ram, size_t sz, uint32_t off) {   // correct 68k word
    if (off + 1 >= sz) return 0;
    return static_cast<int16_t>(ram[off] | (ram[off + 1] << 8));
}
static int be16_direct(const uint8_t* ram, size_t sz, uint32_t off) { // no-swap (diagnostic)
    if (off + 1 >= sz) return 0;
    return static_cast<int16_t>((ram[off] << 8) | ram[off + 1]);
}

// ---- VRAM reads ------------------------------------------------------------
// The fork's VRAM stores each 16-bit word in little-endian host order, so a
// 16/32-bit value at an even offset is read directly (no ^1) — same as the
// scroll-aware resolver in ayther_session.cpp.
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

static int mod_pos(int a, int m) { return m > 0 ? ((a % m) + m) % m : a; }

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
            "mode3_spike — validate VDP scroll reads (risk #2) + Mode 3 anchoring\n"
            "usage: %s <core.dll> <rom> [--warmup N] [--settle N] [--verbose]\n",
            argv[0]);
        return 2;
    }
    const std::string core = argv[1];
    const std::string rom  = argv[2];
    int  warmup  = 1200;  // max frames to reach EHZ Act 1 gameplay (SEGA→title→level)
    int  settle  = 60;    // frames standing still so the camera stops (no 1-frame skew)
    bool verbose = false;
    std::string profile = "tools/mode3_spike/sonic2.toml";  // Mode 3 anchor profile
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--warmup") && i + 1 < argc) warmup = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--settle") && i + 1 < argc) settle = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--profile") && i + 1 < argc) profile = argv[++i];
        else if (!std::strcmp(argv[i], "--verbose")) verbose = true;
    }

    RetroRunner runner;
    if (!runner.init(core, rom)) {
        std::fprintf(stderr, "[modo3] init failed (core/rom path?)\n");
        return 1;
    }
    runner.subscribe_all_supported();   // sin esto el fork no observa nada
    std::fprintf(stdout, "[modo3] core fps=%.4f pixfmt=%u\n", runner.fps(), runner.pixel_format());

    const uint8_t* ram   = runner.work_ram();
    const size_t   ramsz = runner.work_ram_size();

    // ---- Warm-up (adaptive): reach gameplay, then hold RIGHT to scroll ------
    // The SEGA logo / title take a variable number of frames, so we tap START
    // periodically until the player object spawns (its X/Y become non-zero) —
    // rather than guessing a fixed frame. Then we hold RIGHT so the camera
    // builds up a large scroll (makes the sign/offset test meaningful).
    int reached = -1;
    for (int i = 0; i < warmup; ++i) {
        runner.set_input(0, (i % 32 == 0) ? BTN_START : 0);   // periodic tap
        runner.run_frame();
        if (ram && ramsz >= 0x10000
            && be16_bus(ram, ramsz, OFF_SONIC_X) != 0
            && be16_bus(ram, ramsz, OFF_SONIC_Y) != 0) { reached = i; break; }
    }
    if (reached < 0)
        std::fprintf(stdout, "[modo3] WARN: player never spawned within %d frames "
                             "(increase --warmup)\n", warmup);
    else
        std::fprintf(stdout, "[modo3] gameplay reached at warm-up frame %d\n", reached);
    // Hold RIGHT to move the player and scroll the camera off the origin (so the
    // test exercises a large, non-trivial scroll), then RELEASE and let the
    // player settle to a stop. Sampling stationary removes the 1-frame skew
    // between the camera in RAM (read now) and the Hscroll latched into VRAM
    // during the previous frame's render — when Sonic runs at his 6px/frame top
    // speed they differ by ~6px; standing still they agree exactly.
    for (int i = 0; i < 150; ++i)  { runner.set_input(0, BTN_RIGHT); runner.run_frame(); }
    for (int i = 0; i < settle; ++i) { runner.set_input(0, 0); runner.run_frame(); }

    // One more frame standing still — the frame whose RAM/VDP/SAT we read below.
    runner.set_input(0, 0);
    runner.run_frame();

    // ---- Sanity: does the fork expose the VDP memory (risk #2 precondition)?
    size_t vsz = 0, rsz = 0, vssz = 0;
    const uint8_t* vram = runner.video_ram();      vsz  = runner.video_ram_size();
    const uint8_t* regs = runner.vdp_regs();       rsz  = runner.vdp_regs_size();
    const uint8_t* vsram= runner.vsram();          vssz = runner.vsram_size();

    std::fprintf(stdout,
        "[modo3] mem: work_ram=%zuB vram=%zuB vdp_regs=%zuB vsram=%zuB\n",
        ramsz, vsz, rsz, vssz);

    int fail = 0;
    auto check = [&](bool ok, const char* m) {
        std::fprintf(stdout, "  [%s] %s\n", ok ? "ok" : "FAIL", m);
        if (!ok) ++fail;
    };

    std::fprintf(stdout, "\n== Risk #2: VDP memory exposed ==\n");
    check(ram && ramsz >= 0x10000, "work RAM present (>=64KB)");
    check(vram && vsz > 0,  "VRAM present (video_ram id 3)");
    check(regs && rsz >= 0x20, "VDP registers present (id 0x101, >=32)");
    check(vsram && vssz >= 4, "VSRAM present (id 0x107)");
    if (!vram || !regs || !vsram || !ram) {
        std::fprintf(stdout,
            "\n[modo3] core does not expose the VDP memory — need the Ayther fork "
            "(genesis_plus_gx_libretro_vram.dll). ABORT.\n");
        return 3;
    }

    // ---- Plane geometry from the VDP registers -----------------------------
    auto cells = [](int b) { return b == 1 ? 64 : (b == 3 ? 128 : 32); };
    const int wc  = cells(regs[0x10] & 3);
    const int hc  = cells((regs[0x10] >> 4) & 3);
    const int wpx = wc * 8, hpx = hc * 8;

    // ---- Camera scroll from the VDP (Hscroll table in VRAM + VSRAM) --------
    // Hscroll table base = reg $0D << 10; per-line mask from reg $0B. Foreground
    // (Plane A) uses the low word; Plane B the high word. Read at screen line 0
    // (full-screen mode ignores the line anyway). Sign (ayther_session.cpp):
    //   screen_x = plane + H  →  camera_x ≡ −H   (mod wpx)
    //   screen_y = plane − V  →  camera_y ≡ +V   (mod hpx)
    const uint32_t hscb  = ((uint32_t)regs[0x0D] << 10) & 0xFC00;
    const uint32_t hmaskT[4] = { 0x00, 0x07, 0xF8, 0xFF };
    const uint32_t hmask = hmaskT[regs[0x0B] & 3];
    const uint32_t hw0   = vrd32(vram, vsz, hscb + ((0u & hmask) << 2));
    const int H_A = (int)(hw0 & 0x3FF);
    const int H_B = (int)((hw0 >> 16) & 0x3FF);
    const uint32_t vs0 = vsrd32(vsram, vssz, 0);
    const int V_A = (int)(vs0 & 0x3FF);
    const int V_B = (int)((vs0 >> 16) & 0x3FF);

    const int cam_x_vdp = mod_pos(-H_A, wpx);
    const int cam_y_vdp = mod_pos( V_A, hpx);

    // ---- Camera scroll from Sonic 2 work RAM (independent ground truth) -----
    const int cam_x_ram = be16_bus(ram, ramsz, OFF_CAMERA_X);
    const int cam_y_ram = be16_bus(ram, ramsz, OFF_CAMERA_Y);

    // ---- Player world position from work RAM -------------------------------
    const int px_bus = be16_bus(ram, ramsz, OFF_SONIC_X);
    const int py_bus = be16_bus(ram, ramsz, OFF_SONIC_Y);
    const int px_dir = be16_direct(ram, ramsz, OFF_SONIC_X);   // diagnostic
    const int py_dir = be16_direct(ram, ramsz, OFF_SONIC_Y);

    std::fprintf(stdout, "\n== VDP scroll vs game camera ==\n");
    std::fprintf(stdout,
        "  plane: %dx%d cells (%dx%d px)  hscroll_base=0x%04X mode=%u\n",
        wc, hc, wpx, hpx, hscb, regs[0x0B] & 3);
    std::fprintf(stdout,
        "  VDP  : H_A=%d H_B=%d V_A=%d V_B=%d  →  cam_vdp=(%d,%d)\n",
        H_A, H_B, V_A, V_B, cam_x_vdp, cam_y_vdp);
    std::fprintf(stdout,
        "  RAM  : cam=(%d,%d)  cam_ram mod plane=(%d,%d)\n",
        cam_x_ram, cam_y_ram, mod_pos(cam_x_ram, wpx), mod_pos(cam_y_ram, hpx));
    const int vx = be16_bus(ram, ramsz, 0xB014), vy = be16_bus(ram, ramsz, 0xB018);
    std::fprintf(stdout,
        "  player world: bus(LE)=(%d,%d)  direct(BE)=(%d,%d)  vel=(%d,%d)\n",
        px_bus, py_bus, px_dir, py_dir, vx, vy);

    // Cross-check: the VDP-derived camera must equal the game's camera, reduced
    // mod the plane size. A wrong sign/offset in the VDP read fails this.
    auto near_mod = [](int a, int b, int m, int tol) {
        int d = mod_pos(a - b, m);
        return d <= tol || d >= m - tol;
    };
    check(near_mod(cam_x_vdp, cam_x_ram, wpx, 2),
          "VDP Hscroll matches game camera X (mod plane)  → risk #2 X confirmed");
    check(near_mod(cam_y_vdp, cam_y_ram, hpx, 2),
          "VDP VSRAM matches game camera Y (mod plane)    → risk #2 Y confirmed");

    // ---- Load the Mode 3 anchor profile (game_profile.rs) ------------------
    // Declares where the entities live in RAM (Sonic + Tails as one count=2
    // kind). If it fails to load, the probe falls back to hardcoded offsets so
    // the risk-#2 validation still runs.
    AytherGameProfile* prof = ayther_game_profile_load(profile.c_str());
    std::fprintf(stdout, "[modo3] profile: %s → %s\n",
                 profile.c_str(), prof ? "cargado (plomería de Modo 3)" : "NO cargado (fallback hardcodeado)");

    // ---- Mode 3 probe (reused for the stationary + moving frames) ----------
    // Reads the current VDP camera, reads the entities from RAM *through the
    // profile* (ayther_game_profile_entities/_assign), scans this frame's SAT,
    // and assigns. The camera is re-derived from the CURRENT VDP each call (the
    // vram/vsram/regs pointers are stable; only their contents change).
    struct Probe { int sonic_ex, sonic_ey; bool sonic_on; int nspr;
                   int sonic_hits, tails_hits, sep; };
    auto mode3_probe = [&](const char* tag) -> Probe {
        // Camera from the VDP (Hscroll table in VRAM + VSRAM), this frame.
        const uint32_t hw = vrd32(vram, vsz, hscb + ((0u & hmask) << 2));
        const int cvx = mod_pos(-(int)(hw & 0x3FF), wpx);
        const uint32_t vs = vsrd32(vsram, vssz, 0);
        const int cvy = mod_pos((int)(vs & 0x3FF), hpx);
        std::fprintf(stdout, "\n== Mode 3 (%s): entities & SAT ==\n", tag);

        // Entities' world positions — from the profile, else hardcoded fallback.
        std::vector<uint64_t> eid; std::vector<int32_t> ewx, ewy;
        if (prof) {
            uint64_t id[32]; int32_t wx[32], wy[32];
            const size_t ne = ayther_game_profile_entities(prof, ram, ramsz, id, wx, wy, 32);
            for (size_t i = 0; i < ne; ++i) { eid.push_back(id[i]); ewx.push_back(wx[i]); ewy.push_back(wy[i]); }
        } else {
            const uint32_t ox[2] = { OFF_SONIC_X, OFF_TAILS_X }, oy[2] = { OFF_SONIC_Y, OFF_TAILS_Y };
            const uint64_t ids[2] = { 0xB000, 0xB040 };
            for (int i = 0; i < 2; ++i) {
                const int w = be16_bus(ram, ramsz, ox[i]), h = be16_bus(ram, ramsz, oy[i]);
                if (w != 0 || h != 0) { eid.push_back(ids[i]); ewx.push_back(w); ewy.push_back(h); }
            }
        }

        // Project world→screen (for display + separation); track Sonic/Tails.
        std::vector<int> esx(eid.size()), esy(eid.size());
        int son_ex = 0, son_ey = 0; bool son_on = false;
        int tx = INT32_MIN, ty = INT32_MIN;   // Tails screen, if present
        for (size_t i = 0; i < eid.size(); ++i) {
            esx[i] = mod_pos(ewx[i] - cvx, wpx); esy[i] = mod_pos(ewy[i] - cvy, hpx);
            const bool on = esx[i] < 336 && esy[i] < 240;
            std::fprintf(stdout, "  %-5s world=(%d,%d)  →  screen=(%d,%d)%s\n",
                         ent_name(eid[i]), ewx[i], ewy[i], esx[i], esy[i], on ? "" : "  [off-screen]");
            if (eid[i] == 0xB000) { son_ex = esx[i]; son_ey = esy[i]; son_on = on; }
            if (eid[i] == 0xB040) { tx = esx[i]; ty = esy[i]; }
        }

        // SAT: scan the 80 slots LINEARLY from VRAM (base = VDP reg $5 << 9) —
        // NOT following the link chain. Sonic 2 EHZ leaves a *cyclic* chain that
        // traps a naive chain-walk into looping over a few entries and never
        // reaching the player; a linear slot scan sidesteps it. VRAM words are
        // little-endian in the fork buffer (same as the tilemap reader): Y at
        // slot+0, size at slot+2, X at slot+6, read directly. screen = raw−128.
        std::vector<int16_t> sx, sy; std::vector<int> sw, sh;
        const uint32_t satb = (uint32_t)(regs[0x05] & 0x7F) << 9;
        for (int i = 0; i < 80; ++i) {
            const uint32_t b = satb + (uint32_t)i * 8u;
            const int y    = (int)(vrd16(vram, vsz, b)     & 0x3FF) - 128;
            const int x    = (int)(vrd16(vram, vsz, b + 6) & 0x1FF) - 128;
            const int size = (int)vrd16(vram, vsz, b + 2);
            const int w = ((size >> 8) & 3) + 1, h = ((size >> 10) & 3) + 1;  // H/V size
            if (x <= -32 || x >= 336 || y <= -32 || y >= 240) continue;       // off-screen
            sx.push_back((int16_t)x); sy.push_back((int16_t)y);
            sw.push_back(w); sh.push_back(h);
        }
        const size_t nspr = sx.size();
        std::fprintf(stdout, "  SAT sprites = %zu (base=0x%04X)\n", nspr, satb);

        // Assign — through the profile (world→screen + box are inside), or the
        // fallback SoA call. `owner[j]` = id of the entity claiming sprite j.
        std::vector<uint64_t> owner(nspr, 0);
        if (prof) {
            ayther_game_profile_assign(prof, ram, ramsz, cvx, cvy, wpx, hpx,
                                       sx.data(), sy.data(), nspr, owner.data());
        } else {
            std::vector<int32_t> a(nspr, -1);
            ayther_mode3_assign_sprites(eid.data(), esx.data(), esy.data(), eid.size(),
                                        0, 0, 0, 0, 40, 40, sx.data(), sy.data(), nspr, a.data());
            for (size_t j = 0; j < nspr; ++j) if (a[j] >= 0) owner[j] = eid[a[j]];
        }

        std::vector<int> tally(eid.size(), 0);
        for (size_t j = 0; j < nspr; ++j)
            for (size_t k = 0; k < eid.size(); ++k) if (owner[j] == eid[k]) { ++tally[k]; break; }
        for (size_t k = 0; k < eid.size(); ++k)
            std::fprintf(stdout, "  assign_sprites → %-5s claims %d sprite(s)\n",
                         ent_name(eid[k]), tally[k]);
        if (verbose)
            for (size_t j = 0; j < nspr; ++j)
                std::fprintf(stdout, "     sprite[%2zu] top-left=(%4d,%4d) %dx%d  %s\n",
                             j, sx[j], sy[j], sw[j], sh[j], owner[j] ? ent_name(owner[j]) : "");

        auto hits = [&](uint64_t id) {
            for (size_t k = 0; k < eid.size(); ++k) if (eid[k] == id) return tally[k];
            return 0;
        };
        int sep = INT32_MAX;   // Sonic↔Tails screen separation (INT32_MAX = not both present)
        if (son_on && tx != INT32_MIN) {
            const int adx = son_ex - tx < 0 ? tx - son_ex : son_ex - tx;
            const int ady = son_ey - ty < 0 ? ty - son_ey : son_ey - ty;
            sep = adx > ady ? adx : ady;
        }
        return Probe{ son_ex, son_ey, son_on, (int)nspr, hits(0xB000), hits(0xB040), sep };
    };

    // ---- Stationary frame: the rigorous checks (exact VDP scroll) ----------
    const Probe st = mode3_probe("estacionario");
    check(st.sonic_on, "Sonic projects on-screen through the VDP scroll");
    check(st.nspr > 0, "SAT parsed (>=1 hardware sprite)");
    check(st.sonic_hits > 0,
          "Sonic's world_pos + VDP scroll lands on his real SAT sprites  → Mode 3 confirmed");
    if (st.sep != INT32_MAX && st.sep <= 40)
        std::fprintf(stdout,
            "  [info] Sonic y Tails superpuestos (%dpx) parado → el cluster va al más "
            "cercano; la desambiguación se muestra abajo, en movimiento.\n", st.sep);

    // ---- Moving frame: two-entity disambiguation (Sonic ahead, Tails behind)
    // Standing still, Tails catches up and overlaps Sonic. Running RIGHT again
    // pulls them apart, so each character occupies a distinct world_pos and
    // Mode 3 must split the SAT between them. The ~2px camera skew while moving
    // (VRAM Hscroll latched one frame behind RAM) shifts both entities equally,
    // so the RELATIVE split is unaffected.
    for (int i = 0; i < 120; ++i) { runner.set_input(0, BTN_RIGHT); runner.run_frame(); }
    const Probe mv = mode3_probe("en movimiento");
    if (mv.sep != INT32_MAX && mv.sep > 24)
        check(mv.sonic_hits > 0 && mv.tails_hits > 0,
              "Sonic & Tails, separados, reclaman sus propios sprites  → split de Modo 3 confirmado");
    else
        std::fprintf(stdout,
            "  [info] no se separaron lo suficiente para el split este frame; "
            "Sonic sigue validado arriba.\n");

    ayther_game_profile_free(prof);

    // ---- Verdict -----------------------------------------------------------
    std::fprintf(stdout, "\n============================================================\n");
    if (fail == 0) {
        std::fprintf(stdout,
            " OK — VDP scroll read (risk #2) y Modo 3 VALIDADOS contra ROM real.\n"
            "   El scroll de Modo 3 y el eje de Fondos quedan DESBLOQUEADOS.\n");
    } else {
        std::fprintf(stdout,
            " FALLO (%d) — revisar la salida de arriba (warmup insuficiente para\n"
            "   llegar a gameplay, o signo/offset de la lectura de VDP).\n", fail);
    }
    std::fprintf(stdout, "============================================================\n");
    return fail ? 5 : 0;
}
