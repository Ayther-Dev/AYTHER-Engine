// ---------------------------------------------------------------------------
// paint_repalette_smoke (2026-07-31) — el RE-PALETADO de un hash de tile de
// plano es exacto, no una aproximación.
//
// Clonar un Carácter (o un Juego de caracteres entero) a otro color necesita
// los hashes que tendrían SUS MISMOS patrones bajo otra línea de CRAM. La
// tentación es exigir que la variante esté en pantalla para leerlos; no hace
// falta: el hash es `FNV1a(32 bytes del patrón)` con la paleta mezclada AL
// FINAL por un primo impar, así que la última vuelta se puede deshacer y
// rehacer con otra línea. `ayther_plane_tile_hash_repalette` hace eso.
//
// El oráculo NO re-implementa la fórmula: la contrasta contra los hashes que
// el motor ya calculó. Sobre frames reales de una toma, para todo par de
// occurrences con el MISMO `pattern` y distinta `palette`, tiene que valer
//     repalette(a.hash, a.palette, b.palette) == b.hash
// y la vuelta redonda (ida y vuelta) tiene que devolver el original.
//
// Si una toma no ofrece ningún par así, el smoke lo dice y FALLA en vez de
// pasar por vacío — un oráculo que no encontró su caso no probó nada.
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target paint_repalette_smoke
//   Args:  <rec.arp> [f0] [f1]     (default 0..600)
//   Env:   AYTHER_PROBE_ROM
// ---------------------------------------------------------------------------
#include "ayther_env.h"
#include "ayther_session.h"
#include "ayther_recording.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#ifndef AYTHER_SOURCE_DIR
#define AYTHER_SOURCE_DIR "."
#endif

using ayther::FrameView;

static int g_checks = 0, g_fails = 0;
static void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) ++g_fails;
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
}
static std::string toml_quoted(const std::string& l) {
    const auto a = l.find('"'), b = l.rfind('"');
    return (a == std::string::npos || b <= a) ? std::string() : l.substr(a + 1, b - a - 1);
}
static std::string resolve(const std::string& p, const std::string& base) {
    if (p.empty() || (p.size() > 1 && p[1] == ':') || p[0] == '/' || p[0] == '\\') return p;
    return base + "/" + p;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "uso: paint_repalette_smoke <rec.arp> [f0] [f1]\n");
        return 2;
    }
    const std::string rec_path = argv[1];
    const uint32_t f0 = argc > 2 ? (uint32_t)std::strtoul(argv[2], nullptr, 10) : 0u;
    const uint32_t f1 = argc > 3 ? (uint32_t)std::strtoul(argv[3], nullptr, 10) : 600u;

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
    auto rec = ayther::AytherRecording::load(rec_path);
    if (!rec) { std::fprintf(stderr, "[FAIL] no se pudo cargar %s\n", rec_path.c_str()); return 1; }

    std::printf("=== paint_repalette_smoke ===  frames %u..%u\n\n", f0, f1);

    // Los pares se comparan DENTRO de un mismo frame. El índice de patrón NO es
    // una identidad estable en el tiempo: el VDP reescribe la VRAM y el patrón
    // 1042 de un frame puede ser otro dibujo diez frames después. Cruzar frames
    // comparaba gráficos distintos y hacía fallar a la fórmula por un defecto
    // del oráculo (pasó de verdad al escribir esto).
    size_t pairs = 0, bad = 0, patterns_multi = 0, frames = 0;
    uint64_t sample_h = 0;
    int sample_a = 0, sample_b = 0;
    for (uint32_t f = f0; f <= f1; f += 5) {
        const FrameView* fv = s->replay_seek(*rec, f);
        if (!fv) continue;
        ++frames;
        std::map<std::pair<int, int>, std::map<int, uint64_t>> seen;
        for (uint32_t i = 0; i < fv->plane_tile_occ_count; ++i) {
            const auto& o = fv->plane_tile_occs[i];
            seen[{ o.plane, o.pattern }][o.palette] = o.hash;
        }
        for (const auto& [key, bypal] : seen) {
            if (bypal.size() < 2) continue;
            ++patterns_multi;
            for (const auto& [pa, ha] : bypal)
                for (const auto& [pb, hb] : bypal) {
                    if (pa == pb) continue;
                    ++pairs;
                    const uint64_t got = ayther::ayther_plane_tile_hash_repalette(
                        ha, (uint8_t)pa, (uint8_t)pb);
                    if (got != hb) {
                        if (bad < 3)
                            std::printf("    f%u patrón %d (plano %d): pal %d→%d "
                                        "esperado 0x%016llx obtuvo 0x%016llx\n",
                                        f, key.second, key.first, pa, pb,
                                        (unsigned long long)hb,
                                        (unsigned long long)got);
                        ++bad;
                    } else if (!sample_h) {
                        sample_h = ha; sample_a = pa; sample_b = pb;
                    }
                }
        }
    }
    std::printf("frames muestreados: %zu · (patrón,frame) con >1 paleta: %zu · pares: %zu\n\n",
                frames, patterns_multi, pairs);

    // Un oráculo que no encontró su caso no probó nada: eso es un FALLO, no un
    // pase silencioso.
    check(pairs > 0,
          "la toma ofrece pares (mismo patrón, distinta paleta) para contrastar");
    if (pairs > 0) {
        check(bad == 0, "repalette reproduce EXACTO el hash que calculó el motor");
        // Ida y vuelta: volver a la línea de origen devuelve el hash original.
        const uint64_t back = ayther::ayther_plane_tile_hash_repalette(
            ayther::ayther_plane_tile_hash_repalette(sample_h, (uint8_t)sample_a,
                                             (uint8_t)sample_b),
            (uint8_t)sample_b, (uint8_t)sample_a);
        check(back == sample_h, "ida y vuelta devuelve el hash original");
        // Re-paletar a la MISMA línea es la identidad (el clon a su propio
        // color no puede cambiar la identidad del elemento).
        check(ayther::ayther_plane_tile_hash_repalette(sample_h, (uint8_t)sample_a,
                                               (uint8_t)sample_a) == sample_h,
              "re-paletar a la misma línea es la identidad");
    }

    std::printf("\n%d checks, %d fails\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
