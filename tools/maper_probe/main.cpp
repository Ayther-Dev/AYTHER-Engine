// ---------------------------------------------------------------------------
// maper_probe — sonda headless del Maper (plan lab-memory-map-plan.md §4).
//
// Modos:
//   --peek OFF LEN          dump hex de Work RAM tras el boot/run
//   --watch OFF N           muestrea u8[OFF], u8[OFF^1] y u16be[OFF] por frame
//   --experiments OUT.toml  análisis dinámico canónico (M4, §4b): corre
//                           idle/right/left/jump/start × 2 seeds desde un
//                           checkpoint y clasifica offsets por su conducta
//                           (timers monótonos, posición correlacionada con
//                           el input, reactivos) → candidatos con confianza.
//   --press-start / --boot N / --run N / --frames N / --boots A,B
//
// La sesión corre EN ESTE PROCESO: el core es no-reentrante (singleton +
// globals), por eso el Lab lanza maper_probe como SUBPROCESO (AnalysisJob)
// y nunca un hilo. stdout: líneas "PROGRESS i/n" parseables.
//
// Byte-order: todos los offsets en la VISTA 68k (addr^1 del array del core,
// verificado por el spike de este mismo tool contra el timer de Sonic 2).
// BYOR: core y ROM llegan por argumentos; nada se hardcodea.
// ---------------------------------------------------------------------------
#include "ayther_session.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace ayther;

namespace {

/// Estadística incremental por offset u8 (vista 68k) para UN experimento.
struct OffsetStats {
    uint32_t changes = 0;       ///< frames donde cambió
    uint32_t ups     = 0;       ///< transiciones que subieron
    uint32_t downs   = 0;       ///< transiciones que bajaron
    uint8_t  first = 0, last = 0;
};

struct Experiment {
    const char* name;
    uint16_t    mask;   ///< bits libretro crudos (B=0 … RIGHT=7, A=8)
};

constexpr Experiment kExperiments[] = {
    {"idle",  0},
    {"right", 1u << 7},
    {"left",  1u << 6},
    {"jump",  1u << 0},   // B (salto típico en Genesis)
    {"start", 1u << 3},
};
constexpr int kNumExp = 5;

/// u8 en vista 68k desde el array crudo del core.
inline uint8_t v68(const uint8_t* ram, size_t n, uint32_t off) {
    const uint32_t i = off ^ 1u;
    return i < n ? ram[i] : 0;
}

struct Candidate {
    uint32_t    addr = 0;
    std::string kind;       // timer | pos_x | input_react
    std::string type;       // u8 | u16
    float       confidence = 0.f;
    std::string evidence;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "uso: maper_probe <core> <rom> [--boot N] [--press-start] "
                     "[--run N] [--peek OFF LEN] [--watch OFF N]\n"
                     "                 [--experiments OUT.toml] [--frames N] "
                     "[--boots A,B]\n");
        return 2;
    }

    int         boot_frames = -1, run_frames = 0, exp_frames = 360;
    bool        press_start = false;
    long        peek_off = -1, peek_len = 0;
    long        watch_off = -1;
    int         watch_n = 0;
    int         boots[2] = {150, 217};
    uint16_t    hold_mask = 0;   // --hold: input sostenido en --run (debug)
    std::string out_path;
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--boot") && i + 1 < argc) {
            boot_frames = std::atoi(argv[++i]);
        } else if (!std::strcmp(argv[i], "--run") && i + 1 < argc) {
            run_frames = std::atoi(argv[++i]);
        } else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) {
            exp_frames = std::atoi(argv[++i]);
        } else if (!std::strcmp(argv[i], "--press-start")) {
            press_start = true;
        } else if (!std::strcmp(argv[i], "--peek") && i + 2 < argc) {
            peek_off = std::strtol(argv[++i], nullptr, 16);
            peek_len = std::strtol(argv[++i], nullptr, 16);
        } else if (!std::strcmp(argv[i], "--watch") && i + 2 < argc) {
            watch_off = std::strtol(argv[++i], nullptr, 16);
            watch_n   = std::atoi(argv[++i]);
        } else if (!std::strcmp(argv[i], "--experiments") && i + 1 < argc) {
            out_path = argv[++i];
        } else if (!std::strcmp(argv[i], "--boots") && i + 1 < argc) {
            std::sscanf(argv[++i], "%d,%d", &boots[0], &boots[1]);
        } else if (!std::strcmp(argv[i], "--hold") && i + 1 < argc) {
            hold_mask = (uint16_t)std::strtol(argv[++i], nullptr, 16);
        }
    }

    AytherSession::Config cfg;
    cfg.core_path        = argv[1];
    cfg.rom_path         = argv[2];
    cfg.enable_audio     = false;
    cfg.derive_core_pack = false;
    auto sess = AytherSession::create(cfg);
    if (!sess) {
        std::fprintf(stderr, "sesión: %s\n", sess.error.message.c_str());
        return 2;
    }
    AytherSession& s = **sess;

    // Default de boot por modo: los experimentos necesitan title estable
    // (1200); peek/watch conservan el default corto histórico (600).
    if (boot_frames < 0) boot_frames = out_path.empty() ? 600 : 1200;

    // ---- Modos simples: peek / watch ----------------------------------------
    if (out_path.empty()) {
        for (int i = 0; i < boot_frames; ++i) { s.set_input(0, 0); s.step(); }
        if (press_start) {
            for (int i = 0; i < 10; ++i) { s.set_input(0, 1u << 3); s.step(); }
            for (int i = 0; i < 10; ++i) { s.set_input(0, 0);       s.step(); }
        }
        for (int i = 0; i < run_frames; ++i) {
            s.set_input(0, hold_mask);
            s.step();
        }

        size_t ram_size = 0;
        const uint8_t* ram = s.work_ram(&ram_size);
        if (!ram || !ram_size) {
            std::fprintf(stderr, "work_ram no disponible\n");
            return 2;
        }
        std::printf("work_ram: %zu bytes (frames: boot=%d%s run=%d)\n",
                    ram_size, boot_frames, press_start ? "+START" : "",
                    run_frames);

        if (peek_off >= 0) {
            for (long row = peek_off; row < peek_off + peek_len; row += 16) {
                std::printf("%06lX ", 0xFF0000l + row);
                for (long b = row; b < row + 16 && b < (long)ram_size; ++b)
                    std::printf(" %02X", ram[b]);
                std::printf("\n");
            }
        }
        if (watch_off >= 0 && watch_n > 0) {
            std::printf("frame  u8[%04lX]  u8[%04lX^1]  u16be[%04lX]\n",
                        watch_off, watch_off, watch_off);
            for (int i = 0; i < watch_n; ++i) {
                s.set_input(0, 0);
                s.step();
                const uint32_t off = (uint32_t)watch_off;
                std::printf("%5d  %8u  %10u  %11u\n", i, ram[off],
                            ram[off ^ 1u],
                            (unsigned)((ram[off] << 8) | ram[off + 1]));
            }
        }
        return 0;
    }

    // ---- Experimentos canónicos (M4) -----------------------------------------
    // Boot común hasta el primer seed; START para entrar al juego (la mayoría
    // de los juegos quedan en title/demo sin él).
    size_t ram_size = 0;
    const uint8_t* ram = s.work_ram(&ram_size);
    if (!ram || !ram_size) {
        std::fprintf(stderr, "work_ram no disponible\n");
        return 2;
    }
    const uint32_t N = static_cast<uint32_t>(ram_size);

    // stats[seed][exp][offset]
    static std::vector<OffsetStats> stats[2][kNumExp];
    std::vector<uint8_t> snap(N);

    const int total_steps = 2 * kNumExp;
    int       done_steps  = 0;

    for (int seed = 0; seed < 2; ++seed) {
        // Re-arrancar desde reset para cada seed (offset de boot distinto =
        // fase distinta de RNG/contadores) — más limpio que heredar estado.
        s.reset();
        for (int i = 0; i < boot_frames + boots[seed]; ++i) {
            s.set_input(0, 0);
            s.step();
        }

        // Entrada al juego ROBUSTA y agnóstica: START puede caer en intro,
        // title, demo o un menú. Tras cada intento se compara right vs idle
        // desde el mismo checkpoint contando BYTES divergentes: el attract
        // es idéntico (0), un menú diverge en pocos (cursor), el GAMEPLAY en
        // muchos (posición, cámara, objetos). Umbral 48. Hasta 6 intentos.
        std::vector<uint8_t> checkpoint;
        std::vector<uint8_t> ram_right(N);
        bool in_game = false;
        for (int round = 0; round < 6 && !in_game; ++round) {
            for (int i = 0; i < 10; ++i)  { s.set_input(0, 1u << 3); s.step(); }
            for (int i = 0; i < 240; ++i) { s.set_input(0, 0);       s.step(); }
            if (!s.serialize(checkpoint)) {
                std::fprintf(stderr, "serialize falló (seed %d)\n", seed);
                return 2;
            }
            for (int i = 0; i < 90; ++i) { s.set_input(0, 1u << 7); s.step(); }
            std::memcpy(ram_right.data(), ram, N);
            if (!s.unserialize(checkpoint)) return 2;
            for (int i = 0; i < 90; ++i) { s.set_input(0, 0); s.step(); }
            uint32_t diff = 0;
            for (uint32_t i = 0; i < N; ++i) diff += ram_right[i] != ram[i];
            std::fprintf(stderr, "seed %d round %d: %u bytes divergentes\n",
                         seed, round, diff);
            if (diff >= 48) {
                in_game = true;   // el input mueve el MUNDO: gameplay
                if (!s.unserialize(checkpoint)) return 2;
            }
        }
        if (!in_game)
            std::fprintf(stderr,
                         "seed %d: no se logró entrar al juego (los "
                         "experimentos corren igual, sobre el attract)\n",
                         seed);

        for (int e = 0; e < kNumExp; ++e) {
            if (!s.unserialize(checkpoint)) {
                std::fprintf(stderr, "unserialize falló\n");
                return 2;
            }
            auto& st = stats[seed][e];
            st.assign(N, OffsetStats{});
            for (uint32_t off = 0; off < N; ++off) {
                const uint8_t v = v68(ram, N, off);
                st[off].first = st[off].last = v;
            }
            for (int f = 0; f < exp_frames; ++f) {
                s.set_input(0, kExperiments[e].mask);
                s.step();
                for (uint32_t off = 0; off < N; ++off) {
                    const uint8_t now = v68(ram, N, off);
                    OffsetStats&  o   = st[off];
                    if (now != o.last) {
                        ++o.changes;
                        if (now > o.last) ++o.ups;
                        else              ++o.downs;
                        o.last = now;
                    }
                }
            }
            ++done_steps;
            std::printf("PROGRESS %d/%d\n", done_steps, total_steps);
            std::fflush(stdout);
        }
    }

    // ---- Clasificación offline ------------------------------------------------
    // u16 BE por offset par: delta total = (first→last) del par alto/bajo.
    auto delta16 = [&](int seed, int e, uint32_t off) -> int {
        const auto& st = stats[seed][e];
        const int f = (st[off].first << 8) | st[off + 1].first;
        const int l = (st[off].last  << 8) | st[off + 1].last;
        return l - f;
    };
    auto changes_of = [&](int seed, int e, uint32_t off) {
        return stats[seed][e][off].changes + stats[seed][e][off + 1].changes;
    };

    std::vector<Candidate> cands;
    char ev[160];

    for (uint32_t off = 0; off + 1 < N; off += 2) {
        // --- timer/contador: cambia seguido en idle y va casi siempre en la
        // misma dirección (wrap-around tolerado: las vueltas en contra son
        // raras respecto del total), consistente en ambos seeds ---
        for (uint32_t b = off; b <= off + 1; ++b) {
            bool timer = true;
            uint32_t total = 0;
            for (int seed = 0; seed < 2 && timer; ++seed) {
                const OffsetStats& o = stats[seed][0][b];   // idle
                total += o.changes;
                const uint32_t against = std::min(o.ups, o.downs);
                if (o.changes < (uint32_t)exp_frames / 8 ||
                    against > o.changes / 8)
                    timer = false;
            }
            if (timer) {
                Candidate c;
                c.addr = b;
                c.kind = "timer";
                c.type = "u8";
                c.confidence =
                    0.6f + 0.4f * (float)std::min<uint32_t>(total,
                                                            2u * exp_frames) /
                               (2.f * exp_frames);
                std::snprintf(ev, sizeof(ev), "idle: monótono, %u cambios",
                              total);
                c.evidence = ev;
                cands.push_back(std::move(c));
            }
        }

        // --- pos_x: u16 quieto en idle que avanza consistente con RIGHT en
        // ambos seeds y NO con LEFT (en el spawn puede no haber espacio a la
        // izquierda: dl≈0 también vale; dl<0 confirma bidireccional) ---
        {
            const int  dr0 = delta16(0, 1, off), dl0 = delta16(0, 2, off);
            const int  dr1 = delta16(1, 1, off), dl1 = delta16(1, 2, off);
            const uint32_t ci0 = changes_of(0, 0, off);
            const uint32_t ci1 = changes_of(1, 0, off);
            const bool quiet = ci0 <= 4 && ci1 <= 4;
            const bool right_up = dr0 > 64 && dr1 > 64;
            const bool left_not = dl0 < dr0 / 4 && dl1 < dr1 / 4;
            if (quiet && right_up && left_not) {
                Candidate c;
                c.addr = off;
                c.kind = "pos_x";
                c.type = "u16";
                const bool bidir = dl0 < -16 && dl1 < -16;
                const float mag  = (float)std::min(dr0, 2048);
                c.confidence = (bidir ? 0.75f : 0.62f) + 0.2f * mag / 2048.f;
                std::snprintf(ev, sizeof(ev),
                              "right:%+d/%+d left:%+d/%+d idle:%u/%u", dr0,
                              dr1, dl0, dl1, ci0, ci1);
                c.evidence = ev;
                cands.push_back(std::move(c));
            }
        }

        // --- input_react: quieto en idle, cambia con jump/start (2 seeds) ---
        {
            const uint32_t ci = changes_of(0, 0, off) + changes_of(1, 0, off);
            const uint32_t cj = changes_of(0, 3, off) + changes_of(1, 3, off);
            const uint32_t cs = changes_of(0, 4, off) + changes_of(1, 4, off);
            if (ci == 0 && (cj >= 4 || cs >= 4)) {
                Candidate c;
                c.addr = off;
                c.kind = "input_react";
                c.type = "u16";
                c.confidence = 0.5f;
                std::snprintf(ev, sizeof(ev), "idle:0 jump:%u start:%u", cj,
                              cs);
                c.evidence = ev;
                cands.push_back(std::move(c));
            }
        }
    }

    // Recortar a lo útil: mejores 64 por confianza (estable por addr).
    std::sort(cands.begin(), cands.end(),
              [](const Candidate& a, const Candidate& b) {
                  if (a.confidence != b.confidence)
                      return a.confidence > b.confidence;
                  return a.addr < b.addr;
              });
    if (cands.size() > 64) cands.resize(64);

    std::FILE* f = std::fopen(out_path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "no se pudo escribir %s\n", out_path.c_str());
        return 2;
    }
    std::fprintf(f, "# maper_probe - candidatos del análisis dinámico (M4)\n");
    std::fprintf(f, "rom = \"%s\"\n", argv[2]);
    std::fprintf(f, "frames = %d\n", exp_frames);
    for (const auto& c : cands) {
        std::fprintf(f,
                     "\n[[candidate]]\naddr = 0x%04X\nkind = \"%s\"\n"
                     "type = \"%s\"\nconfidence = %.2f\nevidence = \"%s\"\n",
                     c.addr, c.kind.c_str(), c.type.c_str(), c.confidence,
                     c.evidence.c_str());
    }
    std::fclose(f);
    std::printf("DONE %zu candidatos -> %s\n", cands.size(), out_path.c_str());
    return 0;
}
