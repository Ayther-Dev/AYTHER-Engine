// ---------------------------------------------------------------------------
// widescreen_sprites_smoke (#231 EM-8.3) — sprites que entran y salen del área
// nueva, y la basura que NO tiene que entrar con ellos.
//
// LAS DOS MITADES DE LA TAREA.
//
//   1. Un sprite que el VDP recorta contra el borde nativo tiene que dibujarse
//      ENTERO en el área extendida: se lo ve entrar y salir en vez de cortarse
//      en el aire. Sale gratis — el pase de sprites ya lleva el corrimiento del
//      centrado y el canvas es más ancho.
//
//   2. Lo que no sale gratis es lo que aparece de yapa. Estacionar los sprites
//      sin usar justo afuera de la pantalla es un idiom de Genesis: siguen en
//      la SAT, con su patrón, quietos. Ensanchar los pone A LA VISTA.
//
// LA REGLA: un sprite entra si TOCA la pantalla nativa. Los que están enteros
// afuera no se dibujan. No se puede distinguir «está por entrar» de «está
// estacionado» —los dos son un sprite quieto afuera— y de los dos errores
// posibles, mostrar basura es visible y permanente mientras que un sprite que
// aparece unos píxeles tarde no se nota.
//
// EL FENÓMENO SE MIDE, no se supone: la primera parte del oráculo recorre una
// toma real y cuenta cuántos sprites caen enteros afuera de la pantalla nativa
// pero DENTRO del área extendida. Si diera cero, la regla estaría defendiendo
// un caso inventado y habría que sacarla.
//
// LA SEGUNDA PARTE es por píxel, sobre una escena SINTÉTICA — sin ROM. Dos
// sprites del mismo color a la misma altura, uno que toca la pantalla y otro
// que no, colocados para que los DOS caigan dentro del canvas ensanchado. El
// que toca tiene que verse; el otro no. Un oráculo que sólo mirara el primero
// pasaría en verde aunque la regla no existiera.
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target widescreen_sprites_smoke (GPU)
//   Run:   bin/widescreen_sprites_smoke [toma.arp]   (ROM en AYTHER_PROBE_ROM)
//          Sin toma corre sólo la parte sintética.
// ---------------------------------------------------------------------------
#include "ayther_layers.h"
#include "ayther_renderer.h"
#include "ayther_session.h"
#include "ayther_recording.h"
#include "ayther_env.h"
#include "../../tests/support/vulkan_test_context.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#ifndef AYTHER_SOURCE_DIR
#define AYTHER_SOURCE_DIR "."
#endif

using namespace ayther;

namespace {

int g_checks = 0, g_fails = 0;
void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) ++g_fails;
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
}

std::string cfg_quoted(const std::string& l) {
    const size_t a = l.find('"'), b = l.rfind('"');
    return (a == std::string::npos || b <= a) ? std::string()
                                              : l.substr(a + 1, b - a - 1);
}
std::string resolve(const std::string& p, const std::string& base) {
    if (p.empty() || (p.size() > 1 && p[1] == ':') || p[0] == '/' || p[0] == '\\')
        return p;
    return base + "/" + p;
}

constexpr uint32_t kW = 320, kH = 224, kWide = 398;
constexpr int32_t  kDx = (int32_t)(kWide - kW) / 2;   // 39: el centrado

// Los dos sprites, elegidos para que los DOS caigan dentro del canvas
// ensanchado — si uno quedara afuera por geometría, su ausencia no probaría
// nada sobre la regla.
constexpr int32_t kTocaX    = -8;    // x1 = 8 > 0  ⇒ TOCA la pantalla nativa
constexpr int32_t kAparcaX  = -34;   // x1 = -18 < 0 ⇒ entero afuera
constexpr int32_t kAdentroX = 150;   // bien adentro: el control de que la regla
                                     // no toca a los sprites normales
constexpr int32_t kSprW = 16, kSprY = 100;

// El elemento se dibuja como UN quad de 8×8 en (x, y): el ancho declarado es el
// del sprite completo, pero el pase emite la celda. Por eso los testigos van a
// +2 del borde IZQUIERDO y no al centro — a +8 caen justo afuera del quad, y la
// ausencia de color se lee como «la regla se lo comió» cuando en realidad ahí
// nunca se dibujó nada. (Pasó: la primera versión fallaba tres chequeos por
// esto, y los tres parecían defectos del motor.)
constexpr int32_t kTestigo = 2;

struct Px { uint8_t b, g, r; };
Px pixel_at(const uint8_t* bgra, uint32_t w, int x, int y) {
    const size_t i = ((size_t)y * w + x) * 4;
    return Px{ bgra[i + 0], bgra[i + 1], bgra[i + 2] };
}
bool is_red(Px p) { return p.r > 200 && p.g < 60 && p.b < 60; }

}  // namespace

int main(int argc, char** argv) {
    const std::string root = AYTHER_SOURCE_DIR;

    std::printf("=== widescreen_sprites_smoke (#231 EM-8.3) ===\n");

    // ---- 1. EL FENÓMENO, sobre una toma real -------------------------------
    if (argc >= 2) {
        std::string core, rom, line;
        std::ifstream cfg(root + "/tests/test_config.toml");
        while (std::getline(cfg, line))
            if (core.empty() && line.find("core") != std::string::npos &&
                line.find('=') != std::string::npos)
                core = cfg_quoted(line);
        core = resolve(core, root);
        if (const char* e = ayther::env_get("AYTHER_PROBE_ROM")) rom = e;
        if (!core.empty() && !rom.empty()) {
            AytherSession::Config c;
            c.core_path = core; c.rom_path = rom;
            c.enable_audio = false; c.derive_core_pack = false;
            auto r = AytherSession::create(c);
            auto rec_opt = AytherRecording::load(argv[1]);
            if (r && rec_opt) {
                std::unique_ptr<AytherSession>& s = *r;
                const AytherRecording rec = std::move(*rec_opt);
                uint32_t parked = 0, frames_with_parked = 0, touching = 0;
                const uint32_t n = rec.frame_count() < 900 ? rec.frame_count() : 900;
                for (uint32_t f = 0; f < n; f += 3) {
                    const FrameView* fv = s->replay_seek(rec, f);
                    if (!fv) continue;
                    uint32_t found_here = 0;
                    for (uint32_t i = 0; i < fv->scene_count; ++i) {
                        const SceneElement& e = fv->scene[i];
                        if (e.layer != 3) continue;
                        const int32_t x0 = e.x, x1 = e.x + (int32_t)e.w;
                        const bool outside = (x1 <= 0) || (x0 >= (int32_t)kW);
                        const bool inside_extension = (x1 > -kDx) && (x0 < (int32_t)kW + kDx);
                        if (outside && inside_extension) { ++parked; ++found_here; }
                        else if (!outside && (x0 < 0 || x1 > (int32_t)kW)) ++touching;
                    }
                    if (found_here) ++frames_with_parked;
                }
                std::printf("  toma: %u sprites ENTEROS afuera pero dentro del area\n"
                            "        extendida, en %u frames · %u recortados por el borde\n",
                            parked, frames_with_parked, touching);
                check(parked > 0,
                      "el fenomeno EXISTE: hay sprites que el ensanchado revelaria");
            }
        }
    } else {
        std::printf("  (sin toma: se saltea la medicion del fenomeno)\n");
    }

    // ---- 2. LA REGLA, por píxel sobre una escena sintética -------------------
    if (!SDL_Init(SDL_INIT_VIDEO)) { std::fprintf(stderr, "[FAIL] SDL_Init\n"); return 1; }
    SDL_Window* win = SDL_CreateWindow("widescreen_sprites_smoke", 64, 64,
                                       SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (!win) { std::fprintf(stderr, "[FAIL] SDL_CreateWindow\n"); return 1; }
    VulkanTestContext ctx;
    if (!ctx.init(win)) { std::fprintf(stderr, "[FAIL] VulkanTestContext::init\n"); return 1; }

    AytherRenderer renderer;
    const std::string sh = root + "/shaders/";
    // Canvas al ancho LÓGICO y a escala 1: así un píxel del canvas es un píxel
    // del emulador y el testigo se puede ubicar con la cuenta a mano.
    if (!renderer.init(ctx, kWide, kH, sh.c_str()) || !renderer.readback_init(ctx)) {
        std::fprintf(stderr, "[FAIL] renderer\n");
        return 1;
    }

    // Patrón 1 = índice 1 (rojo). El fondo del plano B queda en índice 0 (negro)
    // para que el testigo no dependa de nada más.
    std::vector<uint8_t> vram(0x10000, 0);
    for (int i = 0; i < 32; ++i) vram[1 * 32 + i] = 0x11;
    std::vector<uint8_t> cram(128, 0);
    cram[1 * 2] = 7;   // CRAM: R = bits 0-2 ⇒ 7 = rojo puro

    std::vector<SceneElement> scene;
    auto add_spr = [&](int32_t x, uint64_t hash) {
        SceneElement e{};
        e.x = (int16_t)x; e.y = kSprY;
        e.w = (uint8_t)kSprW; e.h = (uint8_t)kSprW;
        e.pattern = 1; e.layer = 3; e.hash = hash;
        e.slot = (uint8_t)scene.size(); e.chain = (uint8_t)scene.size();
        scene.push_back(e);
    };
    add_spr(kTocaX,   0xA1);
    add_spr(kAparcaX, 0xA2);
    add_spr(kAdentroX, 0xA3);

    std::vector<uint16_t> fb((size_t)kW * kH, 0x0000);

    AytherLayerStack stack;
    FrameView fv{};
    fv.fb_width = kW; fv.fb_height = kH;
    fv.fb_pixels = fb.data(); fv.fb_pitch = kW * 2; fv.fb_format = 2;
    fv.scene = scene.data(); fv.scene_count = (uint32_t)scene.size();
    fv.scene_vram = vram.data(); fv.scene_vram_size = vram.size();
    fv.scene_cram = cram.data(); fv.scene_cram_size = cram.size();
    fv.scene_dirty = 0;

    auto render = [&](uint32_t wide, std::vector<uint8_t>& out) -> bool {
        fv.wide_w = wide;
        const uint8_t* px = renderer.export_frame(ctx, fv, nullptr, true, &stack);
        if (!px) return false;
        out.assign(px, px + (size_t)kWide * kH * 4);
        return true;
    };

    // Dónde cae cada uno en el canvas ensanchado (x lógico = x nativo + 39).
    const int px_toca   = kTocaX   + kDx + kTestigo;
    const int px_aparca = kAparcaX + kDx + kTestigo;
    std::printf("  testigos en el canvas de %u px: toca=%d  aparcado=%d\n",
                kWide, px_toca, px_aparca);

    std::vector<uint8_t> img;
    check(render(kWide, img), "rinde el frame ensanchado");
    if (img.empty()) return 1;

    const Px pt = pixel_at(img.data(), kWide, px_toca,   kSprY + kTestigo);
    const Px pa = pixel_at(img.data(), kWide, px_aparca, kSprY + kTestigo);
    std::printf("        toca = R%u G%u B%u   ·   aparcado = R%u G%u B%u\n",
                pt.r, pt.g, pt.b, pa.r, pa.g, pa.b);

    check(is_red(pt),
          "el sprite que TOCA la pantalla se dibuja ENTERO en el area nueva");
    check(!is_red(pa),
          "el que esta entero afuera NO aparece (la basura estacionada)");

    // CONTROL DE NO VACUIDAD. Los dos testigos tienen que ser alcanzables: si
    // el «aparcado» quedara fuera del canvas por geometria, su ausencia no
    // probaria nada. Se comprueba que el MISMO pixel se pinta cuando el sprite
    // sí toca la pantalla.
    {
        // El de afuera se corre hasta TOCAR, y el otro se saca del medio para
        // que el testigo no pueda dar rojo por el sprite equivocado.
        scene[0].x = 200;
        scene[1].x = (int16_t)kTocaX;
        std::vector<uint8_t> img2;
        check(render(kWide, img2), "rinde el control");
        const Px c = pixel_at(img2.data(), kWide, px_toca, kSprY + kTestigo);
        check(is_red(c),
              "CONTROL: ese mismo sprite SI se dibuja cuando toca la pantalla");
        scene[0].x = (int16_t)kTocaX;
        scene[1].x = (int16_t)kAparcaX;
    }

    // Y sin ensanchar, la regla es VACÍA: su condición pide `logical_w > emu_w_`.
    // El testigo es el sprite de ADENTRO — el de afuera no se veía antes del
    // ensanchado tampoco, así que su ausencia no distingue «la regla no corre»
    // de «nunca se vio». Lo que hay que probar es que un sprite normal sigue
    // dibujándose igual.
    {
        renderer.readback_shutdown(ctx);
        renderer.resize(ctx, kW, kH);
        renderer.readback_init(ctx);
        fv.wide_w = 0;
        const uint8_t* px = renderer.export_frame(ctx, fv, nullptr, true, &stack);
        check(px != nullptr, "rinde el frame SIN ensanchar");
        const bool normal = px && is_red(pixel_at(px, kW, kAdentroX + kTestigo, kSprY + kTestigo));
        bool outside = false;
        if (px)
            for (int x = 0; x < 40 && !outside; ++x)
            if (is_red(pixel_at(px, kW, x, kSprY + kTestigo))) outside = true;
        check(normal, "sin ensanchar un sprite NORMAL se sigue dibujando");
        check(!outside, "y el de afuera sigue sin verse, como siempre");
    }

    renderer.readback_shutdown(ctx);
    renderer.shutdown(ctx);
    ctx.shutdown();
    SDL_DestroyWindow(win);
    SDL_Quit();

    std::printf("\n%d checks, %d fails\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
