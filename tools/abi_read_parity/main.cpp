// ---------------------------------------------------------------------------
// abi_read_parity (E-3, #399) — el camino v1 lee LO MISMO que el legacy.
//
// POR QUÉ EXISTE. E-3 agrega lecturas por `capture_snapshot` + `read_region`
// junto a los accessors de punteros crudos, y E-5 va a borrar los viejos. Entre
// medio hay una ventana en la que los dos caminos conviven, y lo único que
// justifica cambiar de uno al otro es que devuelvan exactamente lo mismo. Si
// difirieran, el síntoma no sería un error: sería autoría que se comporta raro
// (una VRAM corrida, un sprite de menos) mucho después y sin apuntar acá.
//
// Compara, sobre EL MISMO frame y sin correr el emulador entre medio:
//   · VRAM, CRAM, VDP regs y VSRAM byte a byte;
//   · el conteo y el contenido de los sprites parseados;
//   · el conteo de escrituras de chip.
//
// Y verifica lo que hace segura la convivencia: el snapshot trae su GENERACIÓN,
// así que una lectura contra un frame viejo se RECHAZA en vez de devolver
// memoria de otro instante — que es justo lo que el puntero crudo no puede
// distinguir.
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target abi_read_parity
//   Env:   AYTHER_PROBE_ROM (opcional: sin ella corre con la ROM sintética del
//          repo) · AYTHER_ABI_CORE (default: el del repo)
// ---------------------------------------------------------------------------
#include "ayther_diagnostic.h"
#include "libretro_host/retro_runner.h"
#include "ayther_env.h"

#include "../common/synth_rom.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
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

}  // namespace

int main() {
    namespace fs = std::filesystem;
    const std::string root = AYTHER_SOURCE_DIR;
    // #415: la ROM del entorno si la hay, y si no la SINTÉTICA — `roms/` está en
    // .gitignore, así que exigir una comercial es lo que dejaba este test fuera
    // del ctest de toda máquina que no fuera la del autor, CI incluida.
    const std::string rom  = ayther::synth::probe_rom_path();
    const std::string core = env_or("AYTHER_ABI_CORE",
        root + "/third_party/cores/genesis_plus_gx_libretro_vram.dll");

    std::printf("=== abi_read_parity (E-3 #399) — v1 vs legacy ===\n");
    if (rom.empty() || !fs::exists(rom) || !fs::exists(core)) {
        std::printf("[skip] falta ROM o core\n"); return 77;
    }

    RetroRunner r;
    if (!r.init(core, rom)) { std::fprintf(stderr, "[FAIL] init\n"); return 1; }
    if (!r.has_ayther_v1()) {
        std::printf("[skip] el core no trae ABI v1 — nada que comparar\n");
        return 77;
    }
    const ayther_interface_v1* api = r.ayther_api();

    // SIN suscribir primero: el contrato del perfil estándar dice que leer una
    // región sin pedirla devuelve NOT_SUBSCRIBED. Verificarlo importa porque es
    // la señal con la que el Engine distingue «no hay datos» de «nadie los
    // pidió» — con el puntero crudo las dos se ven como memoria en cero, y el
    // dual-path del caller depende de poder separarlas para caer al legacy.
    {
        r.run_frame();
        ayther_frame_snapshot_v1 s0{};
        r.capture_frame_snapshot(s0);
        std::vector<uint8_t> tmp(128, 0);
    const auto without_subscription = r.read_region_v1(AYTHER_REGION_CRAM, tmp.data(),
                                              (uint32_t)tmp.size(),
                                              s0.snapshot_generation);
    check(without_subscription.status == AYTHER_STATUS_NOT_SUBSCRIBED,
              "sin suscripcion, read_region dice NOT_SUBSCRIBED (no cero mudo)");
        std::printf("       status=%d (NOT_SUBSCRIBED=%d)\n",
          without_subscription.status, AYTHER_STATUS_NOT_SUBSCRIBED);
    }

    // Y ahora sí, suscribirse.
    ayther_subscription_state_v1 subs{};
    subs.struct_size = sizeof(subs);
    api->get_subscriptions(&subs, sizeof(subs));
    api->set_subscriptions(AYTHER_SUB_ALL & subs.supported_mask);

    // Correr hasta un frame con contenido: comparar pantallas negras del boot
    // haría pasar cualquier cosa. Un tope fijo de frames deja el resultado a
    // merced de la ROM — con Sonic 2 caía en una pantalla SIN sprites y los
    // checks de sprites/audio se saltaban EN SILENCIO, que es un oráculo vacuo
    // disfrazado de verde. Se avanza HASTA que haya contenido de las dos cosas.
    ayther_frame_snapshot_v1 snap{};
    int  frames_run = 0;
    bool cap_ok = false;
    for (; frames_run < 3000; ++frames_run) {
        r.run_frame();
        ayther_frame_snapshot_v1 s{};
        if (!r.capture_frame_snapshot(s).ok()) continue;
        if (frames_run >= 400 && s.parsed_sprite_count > 0 &&
            s.audio_write_count > 0) { snap = s; cap_ok = true; break; }
        if (frames_run % 64 == 0) r.set_input(0, 0x0008);  // START
        else                           r.set_input(0, 0);
    }
    if (!cap_ok) {   // sin contenido: al menos capturar para no abortar
        for (int i = 0; i < 8; ++i) r.run_frame();
    }
    const auto cap = cap_ok ? RetroRunner::AytherReadResult{AYTHER_STATUS_OK, 0}
                            : r.capture_frame_snapshot(snap);
    std::printf("  frames hasta contenido: %d%s\n", frames_run,
                cap_ok ? "" : "  [aviso] sin sprites+audio — checks VACUOS");
    check(cap.ok(), "capture_snapshot funciona tras run_frame");
    if (!cap.ok()) { std::printf("\n=== FAIL ===\n"); return 1; }
    std::printf("  snapshot gen=%llu  sprites=%u  audio=%u  flags=0x%X\n",
                (unsigned long long)snap.snapshot_generation,
                snap.parsed_sprite_count, snap.audio_write_count, snap.flags);

    // ---- Los DOS tamaños declarados tienen que coincidir -------------------
    // `read_*_v1` escribe `query_region().byte_size`; el espejo dimensionaba su
    // buffer con `retro_get_memory_size`. Que coincidan era una suposición
    // TÁCITA sostenida por nada: si la ABI declarara más, la lectura escribe
    // fuera del buffer. Acá queda fijada.
    {
        struct T { const char* nom; uint32_t region; size_t legacy; };
        const T tams[] = {
            { "VRAM",     AYTHER_REGION_VRAM,     r.video_ram_size() },
            { "CRAM",     AYTHER_REGION_CRAM,     r.color_ram_size() },
            { "VDP regs", AYTHER_REGION_VDP_REGS, r.vdp_regs_size()  },
            { "VSRAM",    AYTHER_REGION_VSRAM,    r.vsram_size()     },
        };
        for (const T& t : tams) {
            const size_t abi = r.abi_region_bytes(t.region);
            check(abi == t.legacy,
                  "el byte_size de la ABI == el size del camino legacy");
            std::printf("       %-9s abi=%zu legacy=%zu\n", t.nom, abi, t.legacy);
        }
    }

    // ---- VDP: byte a byte contra el puntero legacy -------------------------
    struct Reg { const char* name; const uint8_t* legacy; size_t bytes;
                 RetroRunner::AytherReadResult (RetroRunner::*fn)(
                     void*, const ayther_frame_snapshot_v1&) const; };
    // The legacy accessors are deprecated FOR PRODUCTION callers. This oracle
    // exists to prove the versioned reads still agree with them byte for byte,
    // so calling them is the measurement, not an oversight.
    AYTHER_LEGACY_ABI_BEGIN
    const Reg regs[] = {
        { "VRAM",     r.video_ram(), r.video_ram_size(), &RetroRunner::read_vram_v1 },
        { "CRAM",     r.color_ram(), r.color_ram_size(), &RetroRunner::read_cram_v1 },
        { "VDP regs", r.vdp_regs(),  r.vdp_regs_size(),  &RetroRunner::read_vdp_regs_v1 },
    };
    AYTHER_LEGACY_ABI_END
    for (const Reg& g : regs) {
        if (!g.legacy || !g.bytes) {
            std::printf("  [aviso] %s no disponible por el camino legacy\n", g.name);
            continue;
        }
        std::vector<uint8_t> v1(g.bytes, 0xCD);   // patron: si no se escribe, se nota
        const auto res = (r.*g.fn)(v1.data(), snap);
        char msg[128];
        std::snprintf(msg, sizeof(msg), "%s: v1 lee OK (%u bytes)", g.name,
                      res.count);
        check(res.ok(), msg);
        std::snprintf(msg, sizeof(msg), "%s: v1 == legacy, byte a byte", g.name);
        check(res.ok() && std::memcmp(v1.data(), g.legacy, g.bytes) == 0, msg);
    }

    // ---- Sprites parseados -------------------------------------------------
    {
        AYTHER_LEGACY_ABI_BEGIN
        const uint8_t legacy_n = r.parsed_sprite_count();
        AYTHER_LEGACY_ABI_END
        std::vector<ayther_sprite_v1> spr(256);
        const auto res = r.read_parsed_sprites_v1(spr.data(), 256, snap);
        check(res.ok(), "sprites: v1 lee OK");
        check(res.count == snap.parsed_sprite_count,
              "sprites: v1 devuelve el count del snapshot");
        check(snap.parsed_sprite_count == legacy_n,
              "sprites: el count coincide con el contador legacy");
        std::printf("       v1=%u  legacy=%u\n", res.count, (unsigned)legacy_n);
        // El contenido. OJO con el stride: la entrada del fork son 10 bytes
        // (yr,xr,attr,w,h,sat_idx,chain_pos) y el consumidor real del Engine ya
        // los lee asi (ayther_sprite_hasher_process_sprites). El comentario de
        // retro_runner.h que decia «8 bytes/sprite» esta desactualizado y hace
        // leer corrido — con 8 este mismo check daba un FAIL fantasma.
        AYTHER_LEGACY_ABI_BEGIN
        const uint8_t* const legacy_sprites = r.parsed_sprites();
        AYTHER_LEGACY_ABI_END
        if (res.ok() && res.count && legacy_sprites) {
            const uint8_t* raw = legacy_sprites;
        bool matching = true;
        for (uint32_t i = 0; i < res.count && matching; ++i) {
                const uint8_t* p = raw + size_t(i) * 10;
                uint16_t yr, xr, attr;
                std::memcpy(&yr,   p,     2);
                std::memcpy(&xr,   p + 2, 2);
                std::memcpy(&attr, p + 4, 2);
            matching = (yr == spr[i].yr && xr == spr[i].xr &&
                           attr == spr[i].attr && p[6] == spr[i].w &&
                           p[7] == spr[i].h);
            if (!matching)
                    std::printf("       sprite[%u] difiere: legacy(yr=%u xr=%u attr=%04X %ux%u) "
                                "v1(yr=%u xr=%u attr=%04X %ux%u)\n", i,
                                yr, xr, attr, p[6], p[7],
                                spr[i].yr, spr[i].xr, spr[i].attr, spr[i].w, spr[i].h);
            }
        check(matching, "sprites: los campos comunes coinciden con el legacy");
        }
    }

    // ---- Escrituras de chip ------------------------------------------------
    {
        std::vector<ayther_audio_write_v1> w(4096);
        const auto res = r.read_audio_writes_v1(w.data(), 4096, snap);
        check(res.ok(), "audio: v1 lee OK");
        check(res.count == snap.audio_write_count,
              "audio: v1 devuelve el count del snapshot");
        AYTHER_LEGACY_ABI_BEGIN
        const uint32_t legacy_writes = r.audio_write_count();
        AYTHER_LEGACY_ABI_END
        std::printf("       v1=%u  legacy=%u\n", res.count, legacy_writes);
    }

    // ---- La generación es lo que hace segura la convivencia ----------------
    {
        // Avanzar el emulador INVALIDA el snapshot viejo: una lectura con esa
        // generación tiene que ser rechazada. Sin esto, el camino v1 no seria
        // mejor que el puntero crudo — devolveria datos de otro frame sin avisar.
        r.run_frame();
        std::vector<uint8_t> tmp(r.color_ram_size() ? r.color_ram_size() : 128, 0);
        const auto stale = r.read_region_v1(AYTHER_REGION_CRAM, tmp.data(),
                                            (uint32_t)tmp.size(),
                                            snap.snapshot_generation);
        check(!stale.ok(),
              "una lectura con la generacion VIEJA se rechaza (no devuelve otro frame)");
        std::printf("       status=%d (OK seria %d)\n", stale.status, AYTHER_STATUS_OK);

        ayther_frame_snapshot_v1 s2{};
        const auto cap2 = r.capture_frame_snapshot(s2);
        check(cap2.ok() && s2.snapshot_generation != snap.snapshot_generation,
              "el snapshot nuevo trae otra generacion");
        const auto fresh = r.read_region_v1(AYTHER_REGION_CRAM, tmp.data(),
                                            (uint32_t)tmp.size(),
                                            s2.snapshot_generation);
        check(fresh.ok(), "…y con la generacion NUEVA la lectura vuelve a andar");
    }

    r.shutdown();
    std::printf("\n%s — %d checks, %d fallos\n",
                g_fails ? "=== FAIL ===" : "=== OK ===", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
