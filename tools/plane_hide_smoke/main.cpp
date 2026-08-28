// ---------------------------------------------------------------------------
// plane_hide_smoke — prueba de integración de la Fase 2b: ocultar un tile de
// PLANO por hash (id 0x105 del fork). Carga core+ROM reales (tests/test_config.toml),
// graba una toma y verifica los invariantes OBSERVABLES de la supresión por plano:
//
//   1. efecto: ocultar TODOS los tiles de Plano A+B en un frame CAMBIA el
//      framebuffer (el fondo desaparece y se revela lo de atrás).
//   2. reversible: al limpiar los ocultos, el framebuffer vuelve EXACTO al original.
//   3. estable: con la misma máscara, dos re-render del mismo frame dan el mismo
//      framebuffer (determinista; la supresión es produce-only).
//
// La supresión es produce-only y se re-arma desde las occurrences de cada produce,
// así que converge en ≤2 re-render del MISMO frame (igual que el replay pausado del
// Lab). Por eso re-aplicamos con replay_invalidate()+replay_seek() varias veces.
//
//   Build:  -DAYTHER_BUILD_SPIKE=ON  →  target plane_hide_smoke
//   Run:    bin/plane_hide_smoke[.exe]   (toma rutas de tests/test_config.toml)
// ---------------------------------------------------------------------------
#include "ayther_session.h"
#include "ayther_recording.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

#ifndef AYTHER_SOURCE_DIR
#define AYTHER_SOURCE_DIR "."
#endif

using ayther::FrameView;

static std::string quoted(const std::string& line) {
    const auto a = line.find('"');
    const auto b = line.rfind('"');
    if (a == std::string::npos || b <= a) return {};
    return line.substr(a + 1, b - a - 1);
}
static std::string resolve(const std::string& p, const std::string& base) {
    if (p.empty()) return p;
    if (p.size() > 1 && p[1] == ':') return p;
    if (p[0] == '/' || p[0] == '\\')  return p;
    return base + "/" + p;
}

// FNV-1a del framebuffer (lo OBSERVABLE).
static uint64_t fb_hash(const FrameView* fv) {
    if (!fv || !fv->fb_pixels || !fv->fb_height || !fv->fb_pitch) return 0;
    const uint8_t* p = static_cast<const uint8_t*>(fv->fb_pixels);
    uint64_t h = 1469598103934665603ull;
    for (uint32_t y = 0; y < fv->fb_height; ++y)
        for (uint32_t x = 0; x < fv->fb_pitch; ++x) {
            h ^= p[(size_t)y * fv->fb_pitch + x];
            h *= 1099511628211ull;
        }
    return h;
}

int main() {
    const std::string root = AYTHER_SOURCE_DIR;
    const std::string cfg_path = root + "/tests/test_config.toml";
    std::ifstream cfg(cfg_path);
    if (!cfg) {
        std::fprintf(stderr,
            "[skip] no existe %s — copiá tests/test_config.example.toml.\n", cfg_path.c_str());
        return 0;
    }
    std::string core, rom, line;
    bool in_rom = false;
    while (std::getline(cfg, line)) {
        if (line.find("[[rom]]") != std::string::npos) { in_rom = true; continue; }
        if (line.find("core") != std::string::npos && line.find('=') != std::string::npos && core.empty())
            core = quoted(line);
        else if (in_rom && rom.empty() && line.find("path") != std::string::npos)
            rom = quoted(line);
    }
    core = resolve(core, root);
    rom  = resolve(rom,  root);
    if (core.empty() || rom.empty()) {
        std::fprintf(stderr, "[FAIL] config incompleta (core='%s' rom='%s')\n", core.c_str(), rom.c_str());
        return 1;
    }
    std::printf("=== plane_hide_smoke ===\ncore: %s\nrom : %s\n\n", core.c_str(), rom.c_str());

    ayther::AytherSession::Config c;
    c.core_path    = core;
    c.rom_path     = rom;
    c.enable_audio = false;
    auto r = ayther::AytherSession::create(c);
    if (!r) { std::fprintf(stderr, "[FAIL] create: %s\n", r.error.message.c_str()); return 1; }
    std::unique_ptr<ayther::AytherSession>& s = *r;

    // Llevar el juego a GAMEPLAY (los logos/título tienen la pantalla en blanco o
    // backdrop, aunque la nametable esté llena): mashear START un buen rato y luego
    // mover a la derecha para que el fondo (Plano A/B) sea visible y varíe.
    constexpr uint16_t START = 1u << 3;   // RETRO_DEVICE_ID_JOYPAD_START
    constexpr uint16_t RIGHT = 1u << 6;
    for (int i = 0; i < 900; ++i) { s->set_input(0, (i % 24 < 6) ? START : 0); s->step(); }

    s->record_start();
    constexpr int kFrames = 360;
    for (int i = 0; i < kFrames; ++i) {
        uint16_t in_mask = RIGHT;                 // caminar → scroll del fondo
        if (i % 48 < 4) in_mask |= (1u << 0);     // saltar (A) de a ratos
        s->set_input(0, in_mask); s->step();
    }
    s->record_stop();
    ayther::AytherRecording take = s->take_recording();
    std::printf("[ok] toma: %u frames\n", take.frame_count());

    int fail = 0;
    auto check = [&](bool ok, const char* msg) {
        std::printf("[%s] %s\n", ok ? "ok" : "FAIL", msg);
        if (!ok) ++fail;
    };

    // --- CONTROL con la capa-máscara (CU-10, sabido-bueno): encontrar un frame
    // R-5 (#274): los canales de supresión del core MURIERON. El contrato
    // nuevo de "ocultar" es: el fb queda COMPLETO e INVARIANTE (es la fuente
    // de datos de los hashers, no el lienzo) y la visibilidad vive en el
    // COMPOSE (capas del renderer + SceneElement.hidden — el pixel-level lo
    // valida scene_compose_smoke en GPU). Este smoke ahora afirma la muerte
    // del canal y la integridad de la escena bajo cualquier ojo.
    uint32_t F = UINT32_MAX;
    for (uint32_t f = 30; f < take.frame_count(); f += 10) {
        const FrameView* p = s->replay_seek(take, f);
        if (p && p->plane_tile_occ_count > 8) { F = f; break; }
    }
    check(F != UINT32_MAX, "hay un frame con tiles de plano (control)");
    if (F == UINT32_MAX) { std::printf("\nFALLÓ (%d)\n", fail); return 1; }

    s->replay_invalidate();
    const FrameView* fv = s->replay_seek(take, F);
    const uint64_t fbA = fb_hash(fv);
    const uint32_t scene_n = fv->scene_count;
    check(fbA != 0 && scene_n > 0, "el frame publica fb y escena");

    // 1. La máscara de capas ya NO toca el fb (canal 0x102 muerto).
    s->set_layer_mask(0xFF & ~(0x01u | 0x02u));
    uint64_t fbB = 0;
    for (int k = 0; k < 2; ++k) { s->replay_invalidate(); fbB = fb_hash(s->replay_seek(take, F)); }
    check(fbB == fbA, "la máscara de capas NO toca el fb (canal 0x102 muerto)");
    s->set_layer_mask(0xFF);

    // 2. Ocultar TODOS los tiles de plano tampoco (canal 0x105 muerto) — y la
    //    ESCENA sigue completa, con los ocultos marcados (no removidos: la
    //    identidad es del inventario; quién dibuja qué lo decide el compose).
    std::vector<uint64_t> hide;
    {
        std::unordered_set<uint64_t> seen;
        for (uint32_t i = 0; i < fv->plane_tile_occ_count; ++i) {
            const auto& o = fv->plane_tile_occs[i];
            if (o.plane <= 1 && seen.insert(o.hash).second) hide.push_back(o.hash);
        }
    }
    check(!hide.empty(), "hay tiles de Plano A/B para ocultar");
    s->set_plane_tile_hidden(hide.data(), hide.size());
    uint64_t fbC = 0;
    const FrameView* fvh = nullptr;
    for (int k = 0; k < 2; ++k) { s->replay_invalidate(); fvh = s->replay_seek(take, F); fbC = fb_hash(fvh); }
    check(fbC == fbA, "ocultar tiles NO toca el fb (canal 0x105 muerto)");
    check(fvh && fvh->scene_count == scene_n,
          "la escena sigue COMPLETA (ocultar marca, no remueve)");
    {
        std::vector<ayther::SceneElement> inv;
        s->scene_inventory(inv);
        size_t marked = 0, planes = 0;
        for (const auto& e : inv)
            if (e.layer <= 1) { ++planes; marked += e.hidden ? 1u : 0u; }
        std::printf("[..] elementos de plano %zu · marcados hidden %zu\n", planes, marked);
        check(planes > 0 && marked == planes,
              "todos los elementos de A/B quedaron marcados hidden en el inventario");
    }

    // 3. Reversible: limpiar deja el fb y el inventario como al principio.
    s->set_plane_tile_hidden(nullptr, 0);
    uint64_t fbD = 0;
    for (int k = 0; k < 2; ++k) { s->replay_invalidate(); fbD = fb_hash(s->replay_seek(take, F)); }
    check(fbD == fbA, "limpiar los ocultos deja todo como estaba");

    std::printf("\n%s (%d fallos)\n", fail ? "FALLÓ" : "OK", fail);
    return fail ? 1 : 0;
}
