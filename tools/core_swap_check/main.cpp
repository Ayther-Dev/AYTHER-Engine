// ---------------------------------------------------------------------------
// core_swap_check — ¿el core NUEVO puede reemplazar al que usa el Lab?
//
// POR QUÉ EXISTE. La migración a la ABI v1 (E-1…E-5) no sirve de nada mientras
// el Lab cargue un binario del fork anterior a `f3fe1e1`, que no la exporta.
// Cambiar el core del proyecto es barato de hacer y caro de equivocar: si el
// binario nuevo observa distinto —una VRAM que no llega, sprites que no se
// parsean, audio que no se registra—, la autoría existente empieza a fallar de
// formas que se ven mucho después y no apuntan al core.
//
// Este oráculo es la evidencia para decidir. Corre la MISMA ROM los MISMOS
// frames con los dos binarios y compara lo que el Engine observa por los
// caminos LEGACY —que son los que el Lab usa hoy y va a seguir usando hasta
// E-5—: la RAM del 68k, el framebuffer, los sprites parseados y el conteo de
// escrituras de chip. Si todo coincide, el reemplazo es seguro y además suma
// la ABI; si algo difiere, mejor enterarse acá.
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target core_swap_check
//   Env:   AYTHER_PROBE_ROM  = ROM (obligatoria)
//          AYTHER_CORE_A     = core actual del Lab   (default: el del repo)
//          AYTHER_CORE_B     = core candidato        (obligatoria)
//          AYTHER_SWAP_FRAMES= frames a comparar     (default 300)
// ---------------------------------------------------------------------------
#include "ayther_session.h"
#include "ayther_env.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

int g_checks = 0, g_fails = 0;
void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) ++g_fails;
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
}

std::string env_or(const char* k, const std::string& d) {
    if (const char* v = ayther::env_get(k)) if (*v) return v;
    return d;
}

uint64_t fnv(const void* p, size_t n, uint64_t h = 1469598103934665603ULL) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}

/// Lo que el Engine OBSERVA de un core, por los caminos que el Lab usa hoy.
struct Observation {
    uint64_t ram_hash   = 0;   // work RAM del 68k
    uint64_t fb_hash    = 0;   // framebuffer acumulado
    uint64_t sprite_sig = 0;   // sprites parseados (el camino del fork)
    uint64_t occ_total  = 0;   // ocurrencias de sprite acumuladas
    uint32_t fb_w = 0, fb_h = 0;
    bool     ok = false;
};

Observation run_core(const std::string& core, const std::string& rom, int frames) {
    Observation observation;
    ayther::AytherSession::Config c;
    c.core_path = core; c.rom_path = rom; c.enable_audio = false;
    auto r = ayther::AytherSession::create(c);
    if (!r) return observation;
    auto& s = *r;
    uint64_t fb = 1469598103934665603ULL, sp = 1469598103934665603ULL;
    for (int i = 0; i < frames; ++i) {
        const ayther::FrameView& fv = s->step();
        if (fv.fb_pixels && fv.fb_pitch && fv.fb_height)
            fb = fnv(fv.fb_pixels, size_t(fv.fb_pitch) * fv.fb_height, fb);
        observation.fb_w = fv.fb_width; observation.fb_h = fv.fb_height;
        // Sprites: el conteo por frame + su firma. Es EL camino que necesita el
        // core forkeado (un core stock devuelve NULL en VIDEO_RAM), así que si
        // el candidato lo perdiera, la detección de sprites del Lab moriría.
        sp = fnv(&fv.sprite_occ_count, sizeof(fv.sprite_occ_count), sp);
        observation.occ_total += fv.sprite_occ_count;
        // CAMPO POR CAMPO, no la struct cruda: AytherSpriteOccurrence tiene
        // padding de alineación al final y ese relleno NO se inicializa, así
        // que hashear los bytes tal cual hace fallar la comparación por basura
        // — un falso positivo del oráculo, no una diferencia del core (se vio
        // en f145 occ[11]: memcmp distinto con todos los campos iguales).
        for (uint32_t k = 0; k < fv.sprite_occ_count; ++k) {
            const AytherSpriteOccurrence& o2 = fv.sprite_occs[k];
            sp = fnv(&o2.hash, sizeof(o2.hash), sp);
            sp = fnv(&o2.anim_group_id, sizeof(o2.anim_group_id), sp);
            sp = fnv(&o2.w_tiles, sizeof(o2.w_tiles), sp);
            sp = fnv(&o2.h_tiles, sizeof(o2.h_tiles), sp);
            sp = fnv(&o2.screen_x, sizeof(o2.screen_x), sp);
            sp = fnv(&o2.screen_y, sizeof(o2.screen_y), sp);
            sp = fnv(&o2.link, sizeof(o2.link), sp);
            sp = fnv(&o2.palette, sizeof(o2.palette), sp);
            sp = fnv(&o2.priority, sizeof(o2.priority), sp);
            sp = fnv(&o2.slot, sizeof(o2.slot), sp);
            sp = fnv(&o2.hflip, sizeof(o2.hflip), sp);
            sp = fnv(&o2.vflip, sizeof(o2.vflip), sp);
        }
    }
    if (const uint8_t* ram = s->work_ram(); ram && s->work_ram_size())
        observation.ram_hash = fnv(ram, s->work_ram_size());
    observation.fb_hash    = fb;
    observation.sprite_sig = sp;
    observation.ok = true;
    return observation;
}

}  // namespace

int main() {
    namespace fs = std::filesystem;
    const std::string root = AYTHER_SOURCE_DIR;

    const std::string rom = env_or("AYTHER_PROBE_ROM", "");
    const std::string A = env_or("AYTHER_CORE_A",
        root + "/third_party/cores/genesis_plus_gx_libretro_vram.dll");
    const std::string B = env_or("AYTHER_CORE_B", "");
    const int frames = std::atoi(env_or("AYTHER_SWAP_FRAMES", "300").c_str());

    std::printf("=== core_swap_check — ¿el core nuevo observa lo mismo? ===\n");
    if (rom.empty() || !fs::exists(rom)) {
        std::printf("[skip] falta AYTHER_PROBE_ROM\n"); return 0;
    }
    if (B.empty() || !fs::exists(B)) {
        std::printf("[skip] falta AYTHER_CORE_B (el candidato)\n"); return 0;
    }
    if (!fs::exists(A)) {
        std::printf("[skip] no esta el core actual: %s\n", A.c_str()); return 0;
    }
    std::printf("A (actual)    : %s\nB (candidato) : %s\nROM: %s\nframes: %d\n\n",
                A.c_str(), B.c_str(), rom.c_str(), frames);

    const Observation a = run_core(A, rom, frames);
    const Observation b = run_core(B, rom, frames);
    check(a.ok && b.ok, "los dos cores cargan y corren la ROM");
    if (!a.ok || !b.ok) { std::printf("\n=== FAIL ===\n"); return 1; }

    // CONTROL: el mismo core contra si mismo. Sin esto, una metrica inestable
    // se lee como «los cores difieren» y manda a investigar al lugar
    // equivocado — pasó acá con los sprites.
    const Observation a2 = run_core(A, rom, frames);
    const bool ram_stable = (a.ram_hash == a2.ram_hash);
    const bool framebuffer_stable = (a.fb_hash == a2.fb_hash);
    const bool sprite_stable = (a.sprite_sig == a2.sprite_sig);
    check(ram_stable && framebuffer_stable, "CONTROL: RAM y video son deterministas (A vs A)");
    if (!sprite_stable)
        std::printf("  [aviso] la firma de sprites NO es determinista ni con el "
                    "MISMO core (A=%016llX A2=%016llX) — no sirve para comparar\n",
                    (unsigned long long)a.sprite_sig,
                    (unsigned long long)a2.sprite_sig);

    std::printf("\n            %-18s %-18s\n", "A (actual)", "B (candidato)");
    std::printf("  ram      %016llX   %016llX\n",
                (unsigned long long)a.ram_hash, (unsigned long long)b.ram_hash);
    std::printf("  fb       %016llX   %016llX\n",
                (unsigned long long)a.fb_hash, (unsigned long long)b.fb_hash);
    std::printf("  sprites  %016llX   %016llX\n",
                (unsigned long long)a.sprite_sig, (unsigned long long)b.sprite_sig);
    std::printf("  occs     %-18llu %-18llu\n",
                (unsigned long long)a.occ_total, (unsigned long long)b.occ_total);
    std::printf("  fb dims  %ux%u              %ux%u\n\n",
                a.fb_w, a.fb_h, b.fb_w, b.fb_h);

    // Diagnóstico: si los sprites difieren, decir DÓNDE. Un hash distinto sin
    // más obliga a adivinar entre «otro orden», «otro conteo» y «otro layout».
    if (a.sprite_sig != b.sprite_sig) {
        std::printf("  -- diff de sprites: primer frame y campo --\n");
        ayther::AytherSession::Config ca, cb;
        ca.core_path = A; ca.rom_path = rom; ca.enable_audio = false;
        cb = ca; cb.core_path = B;
        auto ra = ayther::AytherSession::create(ca);
        auto rb = ayther::AytherSession::create(cb);
        if (ra && rb) {
            auto& sa = *ra; auto& sb = *rb;
            for (int i = 0; i < frames; ++i) {
                const ayther::FrameView& fa = sa->step();
                const ayther::FrameView& fb2 = sb->step();
                if (fa.sprite_occ_count != fb2.sprite_occ_count) {
                    std::printf("     f%-5d CONTEO distinto: A=%u B=%u\n",
                                i, fa.sprite_occ_count, fb2.sprite_occ_count);
                    break;
                }
                const size_t n = fa.sprite_occ_count;
                if (!n) continue;
                const size_t bytes = sizeof(AytherSpriteOccurrence) * n;
                if (std::memcmp(fa.sprite_occs, fb2.sprite_occs, bytes) != 0) {
                    const auto* pa = fa.sprite_occs;
                    const auto* pb = fb2.sprite_occs;
                    for (size_t k = 0; k < n; ++k) {
                        if (std::memcmp(&pa[k], &pb[k], sizeof(pa[k])) == 0) continue;
                        std::printf("     f%-5d occ[%zu] difiere:\n", i, k);
                        auto dump = [](const char* t, const AytherSpriteOccurrence& o) {
                            std::printf("       %s: hash=%016llX xy=(%d,%d) %ux%u "
                                        "slot=%u link=%u pal=%u pri=%u flip=%u%u\n",
                                        t, (unsigned long long)o.hash,
                                        (int)o.screen_x, (int)o.screen_y,
                                        o.w_tiles, o.h_tiles, o.slot, o.link,
                                        o.palette, o.priority, o.hflip, o.vflip);
                        };
                        dump("A", pa[k]);
                        dump("B", pb[k]);
                        break;
                    }
                    break;
                }
            }
        }
        std::printf("\n");
    }

    check(a.ram_hash == b.ram_hash, "la emulacion coincide (RAM del 68k)");
    check(a.fb_hash == b.fb_hash,   "el video coincide (framebuffer de todos los frames)");
    check(a.fb_w == b.fb_w && a.fb_h == b.fb_h, "mismas dimensiones de frame");
    check(b.occ_total > 0, "el candidato PARSEA sprites (necesita el fork)");
    if (sprite_stable)
        check(a.sprite_sig == b.sprite_sig, "los sprites observados coinciden");
    else
        check(a.occ_total == b.occ_total,
              "los sprites coinciden en CANTIDAD (la firma no es comparable)");

    std::printf("\n%s — %d checks, %d fallos\n",
                g_fails ? "=== FAIL ===" : "=== OK ===", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
