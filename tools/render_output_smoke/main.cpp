// ---------------------------------------------------------------------------
// render_output_smoke — ¿SE VE? El oráculo del camino que el usuario mira.
//
// POR QUÉ EXISTE (#415). El audio tenía este mismo agujero y costó dos días de
// Lab mudo con el ctest en verde: los oráculos que ven el core de verdad piden
// una ROM comercial, viven fuera de la corrida diaria y se ejecutan cuando
// alguien se acuerda. En render son VEINTE herramientas y ninguna diaria.
//
// La receta ya está probada por audio_output_smoke: fabricarse el estímulo. Este
// smoke escribe un 68k que programa el VDP —paleta, tiles, una celda de plano A
// y un sprite— y después verifica DOS cosas sobre el mismo frame:
//
//   1. el FRAMEBUFFER del core: que el píxel de esa celda sea el color que le
//      pusimos, y el del sprite el suyo. Es el camino que termina en pantalla.
//   2. el INVENTARIO de escena (#272): que el motor VEA esos dos elementos y en
//      QUÉ capa. Es el camino que alimenta a Pintar, Dibujar y toda la autoría.
//
// Las dos juntas a propósito: se rompen por separado. Que el frame salga bien no
// dice que el inventario lea la VRAM del fork, y un inventario correcto no dice
// que el VDP haya dibujado.
//
// EL CONTROL. Dos ROMs idénticas salvo el dibujo: la segunda programa el VDP y
// los tiles igual, pero no escribe la nametable ni la SAT. Sus píxeles tienen que
// dar BACKDROP y su inventario, nada. Sin el par, «hay color» puede ser cualquier
// cosa — y la lección cara del 2026-08-13 es justamente ésa: tres veces en un día
// un oráculo pasó midiendo la parte que NO se había roto.
//
// LO QUE NO MIDE. La GPU. El compose vive en Vulkan y no es headless; acá se
// verifica lo que el motor produce ANTES de subirlo (y hay smokes de GPU aparte).
// Que el framebuffer no dependa de una GPU es lo que deja a este oráculo entrar
// al ctest de todos los días, que es el punto entero.
// ---------------------------------------------------------------------------
#include "ayther_session.h"

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

// -- La medición -------------------------------------------------------------
//
// El estímulo es la ROM CANÓNICA del repo (tools/common/synth_rom.h), que dibuja
// una celda de plano A roja y un sprite verde en coordenadas conocidas. Las
// coordenadas de muestreo vienen de ahí: si el estímulo cambia, este oráculo
// mide el lugar nuevo y no una posición copiada que se quedó vieja.
using ayther::synth::kCellX;  using ayther::synth::kCellY;
using ayther::synth::kSprX;   using ayther::synth::kSprY;
using ayther::synth::kBgX;    using ayther::synth::kBgY;

struct Px { int r = -1, g = -1, b = -1; };

/// Un píxel del framebuffer del core, en 8 bits por canal. El formato lo dice el
/// propio frame (el Mega Drive sale en RGB565, pero preguntarlo cuesta nada y el
/// oráculo no tiene por qué saberlo de memoria).
Px px_at(const ayther::FrameView& v, int x, int y) {
    if (!v.fb_pixels || x < 0 || y < 0 ||
        uint32_t(x) >= v.fb_width || uint32_t(y) >= v.fb_height) return {};
    const auto* row = static_cast<const uint8_t*>(v.fb_pixels) + size_t(y) * v.fb_pitch;
    Px p;
    if (v.fb_format == 1) {              // XRGB8888
        uint32_t c = 0;
        std::memcpy(&c, row + size_t(x) * 4, 4);
        p.r = int((c >> 16) & 0xFF); p.g = int((c >> 8) & 0xFF); p.b = int(c & 0xFF);
        return p;
    }
    uint16_t c = 0;
    std::memcpy(&c, row + size_t(x) * 2, 2);
    if (v.fb_format == 0) {              // 0RGB1555
        const int r5 = (c >> 10) & 0x1F, g5 = (c >> 5) & 0x1F, b5 = c & 0x1F;
        p.r = (r5 << 3) | (r5 >> 2); p.g = (g5 << 3) | (g5 >> 2); p.b = (b5 << 3) | (b5 >> 2);
    } else {                             // RGB565 (el default del Genesis)
        const int r5 = (c >> 11) & 0x1F, g6 = (c >> 5) & 0x3F, b5 = c & 0x1F;
        p.r = (r5 << 3) | (r5 >> 2); p.g = (g6 << 2) | (g6 >> 4); p.b = (b5 << 3) | (b5 >> 2);
    }
    return p;
}

/// Un canal «encendido» y uno «apagado». El corte está lejos de los dos lados:
/// el rojo a pleno del Mega Drive llega a ~231 y el canal que no se usa queda en
/// 0, así que cualquier valor intermedio es una conversión rota y tiene que
/// fallar, no redondearse a la respuesta que esperábamos.
bool hi(int c) { return c >= 160; }
bool lo(int c) { return c >= 0 && c <= 60; }

struct Shot {
    Px       cell, spr, bg;
    uint32_t w = 0, h = 0;
    int      fmt = -1;
    bool     ok = false;
    // El inventario del mismo frame, por capa (0=B · 1=A · 2=W · 3=Sprite).
    size_t   inv_total = 0, inv_plane_a = 0, inv_sprites = 0;
    uint64_t cell_hash = 0, spr_hash = 0;
};

/// Corre la ROM unos frames y saca la foto: píxeles del último frame producido
/// + inventario de ESE frame.
Shot measure(const std::string& core, const std::string& rom, int frames) {
    Shot s;
    ayther::AytherSession::Config c;
    c.core_path = core;
    c.rom_path  = rom;
    c.enable_audio = false;      // este oráculo mira, no escucha
    auto sr = ayther::AytherSession::create(c);
    if (!sr) return s;
    std::unique_ptr<ayther::AytherSession>& ses = *sr;

    const ayther::FrameView* v = nullptr;
    for (int f = 0; f < frames; ++f) {
        const ayther::FrameView& fv = ses->step();
        // El core no re-dibuja cuando el frame es idéntico al anterior y ahí
        // fb_pixels viene nulo: hay que quedarse con el último que SÍ trajo
        // imagen, no con el último a secas.
        if (fv.fb_pixels) v = &fv;
    }
    if (!v) return s;

    s.w = v->fb_width; s.h = v->fb_height; s.fmt = v->fb_format;
    s.cell = px_at(*v, kCellX, kCellY);
    s.spr  = px_at(*v, kSprX,  kSprY);
    s.bg   = px_at(*v, kBgX,   kBgY);

    std::vector<ayther::SceneElement> inv;
    s.inv_total = ses->scene_inventory(inv);
    for (const auto& e : inv) {
        if (e.layer == 1) {
            ++s.inv_plane_a;
            if (!s.cell_hash) s.cell_hash = e.hash;
        } else if (e.layer == 3) {
            ++s.inv_sprites;
            if (!s.spr_hash) s.spr_hash = e.hash;
        }
    }
    s.ok = true;
    return s;
}

}  // namespace

int main() {
    std::printf("=== render_output_smoke — ¿lo que sale a pantalla SE VE? ===\n");

    const std::string core =
#ifdef AYTHER_SOURCE_DIR
        std::string(AYTHER_SOURCE_DIR) + "/third_party/cores/genesis_plus_gx_libretro_vram.dll";
#else
        "genesis_plus_gx_libretro_vram.dll";
#endif
    if (!std::filesystem::exists(core)) {
        std::printf("[skip] no está el core del fork: %s\n", core.c_str());
        return 77;
    }

    const auto dir = std::filesystem::temp_directory_path();
    const std::string rom_draw  = (dir / "ayther_render_smoke_draw.md").string();
    const std::string rom_blank = (dir / "ayther_render_smoke_blank.md").string();
    {
        ayther::synth::Rom a("RENDER OUTPUT SMOKE");
        ayther::synth::program_canonical(a, /*draw=*/true);
        ayther::synth::Rom b("RENDER OUTPUT SMOKE");
        ayther::synth::program_canonical(b, /*draw=*/false);
        if (!a.save(rom_draw) || !b.save(rom_blank)) {
            std::fprintf(stderr, "[FAIL] no pude escribir las ROMs sintéticas\n");
            return 1;
        }
    }
    std::printf("  ROM sintética: %s\n", rom_draw.c_str());

    constexpr int kFrames = 8;   // el programa dibuja en el primero; el resto es margen
    const Shot drew  = measure(core, rom_draw,  kFrames);
    const Shot blank = measure(core, rom_blank, kFrames);

    if (!drew.ok || !blank.ok) {
        std::fprintf(stderr, "[FAIL] no se pudo abrir la sesión con el core\n");
        return 1;
    }

    std::printf("\n  frame: %ux%u  formato=%d\n", drew.w, drew.h, drew.fmt);
    std::printf("  píxeles (dibujando):  celda=(%d,%d,%d)  sprite=(%d,%d,%d)  fondo=(%d,%d,%d)\n",
                drew.cell.r, drew.cell.g, drew.cell.b,
                drew.spr.r,  drew.spr.g,  drew.spr.b,
                drew.bg.r,   drew.bg.g,   drew.bg.b);
    std::printf("  píxeles (control)  :  celda=(%d,%d,%d)  sprite=(%d,%d,%d)  fondo=(%d,%d,%d)\n",
                blank.cell.r, blank.cell.g, blank.cell.b,
                blank.spr.r,  blank.spr.g,  blank.spr.b,
                blank.bg.r,   blank.bg.g,   blank.bg.b);

    // -- El framebuffer -------------------------------------------------------
    check(drew.w > 0 && drew.h > 0,
          "NO-VACUIDAD: el core entrega un frame con dimensiones");
    // El backdrop es azul oscuro y no negro justamente para que esto signifique
    // algo: un framebuffer sin tocar (todo ceros) falla acá.
    check(lo(drew.bg.r) && lo(drew.bg.g) && drew.bg.b > 0,
          "EL VDP DIBUJA: el fondo es el BACKDROP que programamos, no ceros");
    check(hi(drew.cell.r) && lo(drew.cell.g) && lo(drew.cell.b),
          "LA CELDA DE PLANO A SALE EN PANTALLA, y con SU color (rojo)");
    check(hi(drew.spr.g) && lo(drew.spr.r) && lo(drew.spr.b),
          "EL SPRITE SALE EN PANTALLA, y con SU color (verde)");
    // El control: mismas coordenadas, misma paleta, mismos tiles cargados — lo
    // único que falta es el dibujo. Sin esto, «hay rojo» podría venir de
    // cualquier lado.
    check(blank.bg.b > 0 && lo(blank.cell.r) && lo(blank.spr.g),
          "CONTROL: sin escribir nametable ni SAT, esos píxeles son backdrop");

    // -- El inventario (#272) -------------------------------------------------
    std::printf("\n  inventario: %zu elementos (plano A=%zu · sprites=%zu) · control=%zu\n",
                drew.inv_total, drew.inv_plane_a, drew.inv_sprites, blank.inv_total);
    // Por capa y no en total: un «hay elementos» lo satisface el plano A solo, y
    // los sprites llegan por otro camino (la SAT parseada del fork). Es la misma
    // trampa que en audio dejaba pasar «el detector ve una nota» con el FM
    // ciego, porque el PSG sobrevivía.
    check(drew.inv_plane_a >= 1,
          "EL INVENTARIO VE LA CELDA DE PLANO A (el camino de Pintar)");
    check(drew.inv_sprites >= 1,
          "…Y EL SPRITE, que llega por la SAT del fork y no por el mismo camino");
    check(drew.cell_hash != 0 && drew.spr_hash != 0,
          "…con IDENTIDAD: sin hash no hay nada que autorar");
    check(drew.cell_hash != drew.spr_hash,
          "…y son elementos DISTINTOS, no el mismo contado dos veces");
    check(blank.inv_sprites == 0,
          "CONTROL: sin SAT escrita el inventario no inventa un sprite");

    std::printf("\n%s — %d checks, %d fallos\n",
                g_fails ? "=== FAIL ===" : "=== OK ===", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
