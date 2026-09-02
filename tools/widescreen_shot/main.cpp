// ---------------------------------------------------------------------------
// widescreen_shot — la EVIDENCIA VISUAL del ensanchado y de las bandas.
//
// POR QUÉ EXISTE. #231 (ensanchado) y #421 (cámara por banda) quedaron con la
// implementación entera y la no-regresión verificada, pero con la misma deuda:
// «el fondo se extiende BIEN» y «las 37 bandas de Sonic quedan alineadas» no
// son afirmaciones que un test pueda hacer. La única verificación posible es
// mirar — y hasta ahora mirar exigía montar el Lab, autorar una Panorámica a
// mano y comparar de memoria.
//
// Esto hace el camino entero headless: barre la toma, ARMA la Panorámica con el
// mismo protocolo que «Crear Panorámica» del Lab (el de pano_sweep_probe), y
// renderiza el MISMO frame dos veces —nativo y ensanchado— a dos PNG. La
// comparación sigue siendo a ojo; montarla ya no cuesta nada.
//
//   Build: -DAYTHER_BUILD_SPIKE=ON  ->  target widescreen_shot
//   Run:   bin/widescreen_shot <toma.arp> <f0> <f1> <plano 0|1> <frame> [ancho]
//          core desde tests/test_config.toml · ROM en AYTHER_PROBE_ROM
//
// El ancho por defecto es 398: 16:9 en píxel cuadrado sobre 224 líneas. El otro
// candidato es 427 (preservando el 4:3 mostrado, que es lo que se ve en un CRT);
// los dos los calcula widescreen_target_width() y su test los fija.
//
// QUÉ MIRAR, que es el punto de la herramienta:
//   - widescreen_off.png / widescreen_on.png, el MISMO frame. En el «on» el
//     juego queda CENTRADO y a los lados aparece arte de la lámina. Si los
//     laterales salen negros, la Panorámica no ancló (se avisa por consola).
//   - La costura: el arte lateral tiene que continuar el del centro sin salto.
//     Un corte vertical en el borde del área nativa es el defecto a buscar.
//
// LA CALIDAD DEL LATERAL ES LA DE LA LÁMINA, no la del render. Medido sobre
// Sonic 3 & K con el boot desatendido: el quad se ensancha exacto (320 → 398 px
// con la UV escalada en la misma proporción, 0,0313 → 0,0390), o sea que el
// muestreo es contiguo y correcto; pero la lámina que sale de un barrido que
// CRUZA ZONAS apila dos niveles en el mismo espacio (~2,4 hashes por posición) y
// entonces los laterales traen arte del otro. No es un defecto del ensanchado:
// un autor barre una zona a propósito, y este arnés no puede.
//
//   - Con line-scroll (#421), cada banda se ancla por separado: el síntoma de
//     que el voto por banda funciona es que NINGUNA franja horizontal queda
//     corrida respecto de las de arriba y abajo.
// ---------------------------------------------------------------------------
#include "ayther_session.h"
#include "ayther_recording.h"
#include "ayther_renderer.h"
#include "ayther_env.h"
#include "../../tests/support/vulkan_test_context.h"

#include <SDL3/SDL.h>

// La TU de stb_image_write ya la aporta ayther_engine (tile_tex_cache):
// definir la implementacion aca duplica cada simbolo al linkear.
#include <stb_image_write.h>

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

/// BGRA del readback -> PNG RGBA. El readback es BGRA; confundirlo deja una
/// imagen con los canales cruzados que parece un defecto de render — la trampa
/// que ya está anotada en los oráculos de Acetatos.
bool write_png_bgra(const std::string& path, const uint8_t* bgra, int w, int h) {
    if (!bgra) return false;
    std::vector<uint8_t> rgba((size_t)w * h * 4);
    for (size_t i = 0; i < rgba.size(); i += 4) {
        rgba[i + 0] = bgra[i + 2];
        rgba[i + 1] = bgra[i + 1];
        rgba[i + 2] = bgra[i + 0];
        rgba[i + 3] = 255;              // el offscreen no lleva alfa útil
    }
    return stbi_write_png(path.c_str(), w, h, 4, rgba.data(), w * 4) != 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 6) {
        std::fprintf(stderr,
            "uso: widescreen_shot <toma.arp> <f0> <f1> <plano 0|1> <frame> [ancho=398]\n"
            "  f0..f1 = tramo a barrer para armar la Panoramica\n"
            "  frame  = el que se renderiza (conviene que caiga dentro del tramo)\n");
        return 2;
    }
    const std::string root = AYTHER_SOURCE_DIR;
    std::string core, rom, line;
    {
        std::ifstream cfg(root + "/tests/test_config.toml");
        while (std::getline(cfg, line))
            if (core.empty() && line.find("core") != std::string::npos &&
                line.find('=') != std::string::npos)
                core = cfg_quoted(line);
    }
    core = resolve(core, root);
    if (const char* er = ayther::env_get("AYTHER_PROBE_ROM")) rom = er;
    if (core.empty() || rom.empty()) {
        std::fprintf(stderr,
            "[FAIL] falta core (tests/test_config.toml) o AYTHER_PROBE_ROM\n");
        return 2;
    }

    const std::string rec_path = argv[1];
    uint32_t f0 = (uint32_t)std::atoi(argv[2]);
    uint32_t f1 = (uint32_t)std::atoi(argv[3]);
    const uint8_t  plane = (uint8_t)(std::atoi(argv[4]) & 1);
    const uint32_t frame = (uint32_t)std::atoi(argv[5]);
    const uint32_t wide  = argc > 6 ? (uint32_t)std::atoi(argv[6]) : 398u;

    AytherSession::Config c;
    c.core_path = core; c.rom_path = rom;
    c.enable_audio = false; c.derive_core_pack = false;
    auto r = AytherSession::create(c);
    if (!r) {
        std::fprintf(stderr, "[FAIL] sesion: %s\n", r.error.message.c_str());
        return 1;
    }
    std::unique_ptr<AytherSession>& s = *r;

    // La toma puede ser un .arp del proyecto o el BOOT grabado en vivo ("-",
    // el mismo truco de hscroll_bands_probe). Hace falta para los juegos de los
    // que no existe toma: Golden Axe scrollea reescribiendo VRAM, así que su
    // lámina nunca pasa de una pantalla y el área extendida queda vacía por
    // falta de arte, no por un defecto. Para ver el ensanchado LLENO hay que
    // llegar a un juego que scrollee por registros — y de ésos no hay .arp.
    AytherRecording rec;
    if (rec_path == "-") {
        const int nb = (int)(f1 > 0 ? f1 : 5400);
        std::printf("boot en vivo: %d frames\n", nb);
        s->record_start();
        for (int i = 0; i < nb; ++i) { s->set_input(0, 0); s->step(); }
        s->record_stop();
        rec = s->take_recording();
        // El barrido arranca DENTRO del nivel. Antes está el logo y el título,
        // y la transición dispara el corte de escena (#161) que congela el
        // stitch: la lámina saldría del título. `scene_dirty` bit2 —hscroll por
        // línea con variación real— es la señal de que ya estamos en el nivel.
        f0 = 0;
        for (uint32_t f = 0; f < rec.frame_count(); ++f) {
            const FrameView* fv = s->replay_seek(rec, f);
            if (fv && (fv->scene_dirty & 4)) { f0 = f + 60; break; }
        }
        if (!f0) {
            std::fprintf(stderr, "[FAIL] el boot no llego a un nivel con scroll\n");
            return 1;
        }
        f1 = rec.frame_count() - 1;
        std::printf("nivel desde f%u\n", f0);
    } else {
        auto rec_opt = AytherRecording::load(rec_path);
        if (!rec_opt) {
            std::fprintf(stderr, "[FAIL] no abre %s\n", rec_path.c_str());
            return 1;
        }
        rec = std::move(*rec_opt);
    }
    if (f1 >= rec.frame_count()) f1 = rec.frame_count() - 1;
    if (f0 > f1) { std::fprintf(stderr, "[FAIL] tramo vacio\n"); return 2; }

    std::printf("=== widescreen_shot (#231 / #421) ===\n"
                "toma %s (%u frames) - barrido %u..%u plano %c - frame %u - ancho %u\n\n",
                rec_path.c_str(), rec.frame_count(), f0, f1, plane ? 'B' : 'A',
                frame, wide);

    // -- 1. Armar la Panorámica ------------------------------------------------
    // Mismo protocolo que «Crear Panorámica» del Lab: posicionarse, capturar
    // SECUENCIAL (el stitcher desenrolla el scroll frame a frame; un salto lo
    // rompe), exportar la lámina y declararla.
    s->replay_seek(rec, f0);
    s->bg_capture(true);
    for (uint32_t f = f0; f <= f1; ++f)
        if (!s->replay_seek(rec, f)) {
            std::fprintf(stderr, "[FAIL] seek %u durante el barrido\n", f);
            return 1;
        }
    s->bg_capture(false);


    const std::string png = root + "/build/widescreen_shot_pano.png";
    auto w = s->export_background_plane(plane, png);
    if (!w) {
        std::fprintf(stderr, "[FAIL] export lamina: %s\n", w.error.message.c_str());
        return 1;
    }

    // #349: los bounds del STITCHER son la autoridad — son los del PNG. Usar los
    // extents de bg_cells() puede diferir en una fila/columna (celdas de borde
    // parciales) y ese desfase estira y corre el asset al dibujar.
    int32_t bb[4];
    if (!s->bg_bounds(plane, bb)) {
        std::fprintf(stderr, "[FAIL] el plano %c no acumulo celdas\n",
                     plane ? 'B' : 'A');
        return 1;
    }
    const auto cells = s->bg_cells(plane);
    const uint16_t wc = (uint16_t)(bb[2] - bb[0] + 1);
    const uint16_t hc = (uint16_t)(bb[3] - bb[1] + 1);
    std::printf("lamina: %s\n  %zu celdas - %ux%u celdas (%d..%d, %d..%d)\n\n",
                png.c_str(), cells.size(), wc, hc, bb[0], bb[2], bb[1], bb[3]);
    // WS_NO_PANO=1 corre el ensanchado SIN tira: los lados quedan vacíos a
    // propósito. Separa «el ensanchado rompe la escena nativa» de «la tira se
    // dibuja mal», que a ojo se confunden — el primer diagnóstico de esta
    // herramienta atribuyó a la tira unos sprites que faltaban por otra causa.
    const bool no_pano = ayther::env_get("WS_NO_PANO") != nullptr;
    if (no_pano)
        std::printf("WS_NO_PANO: sin Panoramica (los lados quedan vacios)\n\n");
    else
        s->define_panorama(0x5CEE11u, plane, bb[0], bb[1], wc, hc,
                           cells.data(), (uint32_t)cells.size(), png);

    // Qué frames del tramo componen la escena. #231 y #421 trabajan sobre la
    // escena COMPUESTA; con `scene_dirty` el render cae al híbrido, que dibuja
    // la tira plana encima de todo y no es el camino que ninguna de las dos
    // toca. Fotografiar un frame sucio le atribuye al ensanchado un defecto del
    // fallback — que fue exactamente el primer diagnóstico equivocado de esta
    // herramienta.
    uint32_t shot_frame = frame;
    {
        std::vector<uint32_t> clean_frames;
        uint8_t bits = 0;
        for (uint32_t f = f0; f <= f1; ++f) {
            const FrameView* fv = s->replay_seek(rec, f);
            if (!fv) continue;
            bits |= fv->scene_dirty;
            // Hacen falta LAS DOS COSAS: escena compuesta (si no, el render cae
            // al híbrido, que no es el camino que #231 toca) y la tira ANCLADA
            // (si no, el área extendida queda vacía y la foto no muestra nada).
            // Pedir sólo la primera daba frames con los lados en negro, que se
            // leen como un defecto del ensanchado y no lo son.
            if (fv->scene_dirty == 0 && fv->panorama_valid &&
                fv->panorama_sub_count) clean_frames.push_back(f);
        }
        std::printf("compuesta Y anclada en %zu de %u frames del tramo"
                    " (bits sucios vistos: 0x%02X)\n",
                    clean_frames.size(), f1 - f0 + 1, bits);
        // WS_ALLOW_DIRTY deja fotografiar igual. Sirve para los juegos con
        // line-scroll (#421), que por el bit2 de `scene_dirty` NUNCA componen:
        // ahí la tira se dibuja plana sobre el blit y tapa HUD y sprites, pero
        // las bandas y sus cámaras se ven igual en la consola.
        if (clean_frames.empty()) {
            if (!ayther::env_get("WS_ALLOW_DIRTY")) {
                std::fprintf(stderr,
                    "[FAIL] ningun frame del tramo compone: el render caeria al\n"
                    "       hibrido, que no es el camino que #231 toca. Con\n"
                    "       WS_ALLOW_DIRTY=1 se fotografia igual (util para ver\n"
                    "       las bandas de #421 en un juego con line-scroll).\n");
                return 1;
            }
            std::printf("WS_ALLOW_DIRTY: se fotografia f%u con la escena SUCIA\n",
                        frame);
            clean_frames.push_back(frame);
        }
        // El frame pedido puede caer en uno sucio; se corre al limpio MÁS
        // CERCANO y se dice. Callarlo dejaría la foto en el camino equivocado.
        uint32_t best = clean_frames.front();
        for (uint32_t f : clean_frames)
            if ((f > frame ? f - frame : frame - f) <
                (best > frame ? best - frame : frame - best)) best = f;
        if (best != frame)
            std::printf("f%u no compone; se fotografia f%u, el limpio mas cercano\n",
                        frame, best);
        shot_frame = best;
    }

    // -- 2. Vulkan -------------------------------------------------------------
    if (!SDL_Init(SDL_INIT_VIDEO)) { std::fprintf(stderr, "[FAIL] SDL_Init\n"); return 1; }
    SDL_Window* win = SDL_CreateWindow("widescreen_shot", 64, 64,
                                       SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (!win) { std::fprintf(stderr, "[FAIL] SDL_CreateWindow\n"); return 1; }
    VulkanTestContext ctx;
    if (!ctx.init(win)) { std::fprintf(stderr, "[FAIL] VulkanTestContext::init\n"); return 1; }

    AytherRenderer renderer;
    const std::string sh = root + "/shaders/";
    if (!renderer.init(ctx, 320 * 3, 224 * 3, sh.c_str())) {
        std::fprintf(stderr, "[FAIL] renderer.init\n");
        return 1;
    }

    // El canvas se dimensiona PROPORCIONAL al ancho lógico de cada toma: si los
    // dos salieran del mismo target, el ensanchado se vería aplastado y la
    // comparación mediría la escala en vez del contenido.
    auto shot = [&](const std::string& path, uint32_t logical) -> bool {
        const uint32_t lw = logical ? logical : 320u;
        if (!renderer.resize(ctx, lw * 3, 224 * 3)) return false;
        if (!renderer.readback_init(ctx)) return false;

        s->set_widescreen(logical);
        s->replay_invalidate();
        // PRE-ROLL. Un seek deja la escena SUCIA y el render cae al camino
        // híbrido: ahí la tira se dibuja plana ENCIMA de todo (comportamiento
        // «aproximado» de siempre) y tapa HUD y sprites. Ese no es el camino que
        // #231 y #421 modifican — los dos trabajan sobre la escena COMPUESTA,
        // donde la tira va inline en el z de su plano. Llegar caminando desde
        // unos frames antes la limpia; sin esto, la herramienta fotografía el
        // fallback y se le atribuye al ensanchado un defecto que no es suyo.
        // 90 frames y no 16: el anclaje de la Panoramica tiene regla de
        // CONTINUIDAD (entre candidatos parejos gana el mas cercano al
        // anclaje anterior), asi que arrastra historia. Con pre-roll corto
        // las dos tomas llegaban con historia distinta y la camara salia
        // 23 en una y 31 en la otra — una diferencia del ARNES que se leia
        // como un defecto del ensanchado.
        const uint32_t pre = shot_frame > 90 ? 90u : shot_frame;
        s->replay_seek(rec, shot_frame - pre);
        const FrameView* fv = nullptr;
        for (uint32_t f = shot_frame - pre; f <= shot_frame; ++f) {
            fv = s->replay_seek(rec, f);
            if (!fv) { std::fprintf(stderr, "[FAIL] seek f%u\n", f); return false; }
        }

        // Una Panorámica que no ancla no dibuja NADA a los lados, y el PNG sale
        // con bandas negras que parecen un defecto de render. Decirlo evita
        // atribuirle al ensanchado un problema del anclaje.
        std::printf("  wide_w=%-4u pano=%s (%u/%u votos, cobertura %u%%,"
                    " tira limpia %u%%) cam=(%d,%d) escena=%s"
                    " - %u quads en el plano %c\n",
                    fv->wide_w, fv->panorama_valid ? "ANCLADA" : "SIN ANCLAR",
                    fv->panorama_votes, fv->panorama_cells, fv->panorama_cover,
                    fv->panorama_clean,
                    fv->panorama_cam_x, fv->panorama_cam_y,
                    fv->scene_dirty ? "SUCIA (fallback)" : "compuesta",
                    fv->panorama_sub_count, fv->panorama_plane ? 'B' : 'A');
        // El rect y las UV de cada quad: si el ensanchado los corre mal, acá se
        // ve en números y no hay que deducirlo de la imagen.
        for (uint32_t q = 0; q < fv->panorama_sub_count && q < 8; ++q) {
            const AytherSpriteSub& sq = fv->panorama_subs[q];
            std::printf("    quad %u: rect (%d,%d) %ux%u  uv (%.4f,%.4f) %.4fx%.4f\n",
                        q, sq.screen_x, sq.screen_y, sq.w_px, sq.h_px,
                        sq.u0, sq.v0, sq.uw, sq.vh);
        }

        // La lámina decodifica ASÍNCRONA: la primera pasada sale sin textura y
        // la tira no se dibuja. Sin esperarla, las dos tomas difieren en 616.770
        // píxeles por el decode y no por el ensanchado — o sea que la
        // comparación mediría el warmup. Se renderiza hasta que dos readbacks
        // consecutivos coinciden.
        const VkExtent2D e = renderer.framebuffer_extent();
        const size_t nbytes = (size_t)e.width * e.height * 4;
        std::vector<uint8_t> prev, cur;
        int settle = 0;
        for (; settle < 120; ++settle) {
            const uint8_t* px = renderer.export_frame(ctx, *fv, s->pack(), true);
            if (!px) { std::fprintf(stderr, "[FAIL] export_frame\n"); return false; }
            cur.assign(px, px + nbytes);
            if (!prev.empty() && prev == cur) break;
            prev.swap(cur);
        }
        if (settle >= 120) {
            std::fprintf(stderr, "[FAIL] el frame no se estabilizo en 120 pasadas\n");
            return false;
        }
        std::printf("  estabilizado en %d pasadas\n", settle);
        const bool ok = write_png_bgra(path, prev.data(), (int)e.width, (int)e.height);
        std::printf("  %s %s (%ux%u)\n\n", ok ? "[ok]  " : "[FAIL]",
                    path.c_str(), e.width, e.height);
        renderer.readback_shutdown(ctx);
        return ok;
    };

    const std::string p_off = root + "/build/widescreen_off.png";
    const std::string p_on  = root + "/build/widescreen_on.png";
    std::printf("nativo:\n");
    const bool a = shot(p_off, 0);
    std::printf("ensanchado:\n");
    const bool b = shot(p_on, wide);

    renderer.shutdown(ctx);
    ctx.shutdown();
    SDL_DestroyWindow(win);
    SDL_Quit();
    if (!a || !b) return 1;

    std::printf("Los dos PNG son el MISMO frame. En el ensanchado el juego queda\n"
                "centrado y los lados los llena la lamina: lo que hay que mirar es\n"
                "que el arte lateral CONTINUE el del centro, sin corte vertical en\n"
                "el borde del area nativa ni franjas horizontales corridas.\n");
    return 0;
}
