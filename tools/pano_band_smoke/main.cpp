// ---------------------------------------------------------------------------
// pano_band_smoke (#421) — la Panorámica emite UN QUAD POR BANDA, sobre el
// juego donde el caso EXISTE.
//
// POR QUÉ EXISTE. #421 tiene `tests/pano_bands_test`, que fija el VOTO como
// función pura: agrupa y decide sin leer VRAM ni tocar Vulkan. Eso deja sin
// verificar la mitad que importa —que las has_bands REALES del VDP lleguen al
// voto y salgan como quads distintos— y la issue quedó abierta pidiendo que
// alguien mirara Sonic 3 & Knuckles a ojo.
//
// EL CORPUS ESTÁ MEDIDO, no supuesto (2026-08-24, hscroll_bands_probe):
//
//   Golden Axe   3 tomas, 40.854 frames   reg $B modo 0   0 has_bands
//   Ecco         1.800 frames             reg $B modo 0   0 has_bands
//   Aladdin      1.800 frames             reg $B modo 0   0 has_bands
//   Sonic 3 & K  1.800 frames             tabla por línea en 1.766
//                                         plano A: 1 banda · plano B: 37
//
// Por eso el oráculo corre contra Sonic 3 & Knuckles. Validarlo con Golden Axe
// —el ejemplo que la issue traía— habría dado verde sobre un juego donde el
// defecto no ocurre.
//
// NO NECESITA UNA TOMA DEL PROYECTO. El boot se graba en vivo (el mismo truco
// de hscroll_bands_probe): más reproducible que depender de un .arp del usuario,
// y no existe ninguno de Sonic 3 & K.
//
// DÓNDE EMPIEZA EL BARRIDO, que es lo único delicado. Capturar desde el frame 0
// NO funciona y la primera versión de este oráculo se equivocó ahí: el arranque
// pasa por el logo y el título —pantallas estáticas, sin line-scroll— y la
// transición al level dispara el corte de escena de #161, que CONGELA el stitch.
// Todo lo capturado quedaba del lado del título y las has_bands del otro, así que
// la intersección era CERO y parecía un defecto del motor. Lo era del arnés: un
// autor en el Lab tampoco autora la Panorámica de una transición. Acá se busca
// el primer frame con `scene_dirty` bit2 —la señal de que hay line-scroll, o
// sea de que estamos en el level— y se captura desde ahí.
//
// LOS DOS EXTREMOS, que es lo que lo hace no vacío:
//   · CON has_bands   ⇒ más de un quad, y con CÁMARAS DISTINTAS. Que sean varios
//                    no alcanza: una tira recortada por el borde también da
//                    varios. Lo que prueba el parallax es que difieran en x.
//   · SIN has_bands   ⇒ exactamente el comportamiento viejo. Es la garantía que
//                    hace seguro el cambio para los 40.854 frames de Golden Axe.
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target pano_band_smoke
//   Run:   bin/pano_band_smoke <rom> [frames_boot=1800]
//          (core desde tests/test_config.toml)
// ---------------------------------------------------------------------------
#include "ayther_session.h"
#include "ayther_recording.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <set>
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

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "uso: pano_band_smoke <rom> [frames_boot=1800]\n");
        return 2;
    }
    const std::string root = AYTHER_SOURCE_DIR;
    std::string core, line;
    {
        std::ifstream cfg(root + "/tests/test_config.toml");
        while (std::getline(cfg, line))
            if (core.empty() && line.find("core") != std::string::npos &&
                line.find('=') != std::string::npos)
                core = cfg_quoted(line);
    }
    core = resolve(core, root);
    if (core.empty()) { std::fprintf(stderr, "[skip] sin core\n"); return 0; }

    const int boot = argc >= 3 ? std::atoi(argv[2]) : 1800;

    AytherSession::Config c;
    c.core_path = core; c.rom_path = argv[1];
    c.enable_audio = false; c.derive_core_pack = false;
    auto r = AytherSession::create(c);
    if (!r) {
        std::fprintf(stderr, "[FAIL] sesion: %s\n", r.error.message.c_str());
        return 1;
    }
    std::unique_ptr<AytherSession>& s = *r;

    std::printf("=== pano_band_smoke (#421) — un quad POR BANDA ===\n");
    std::printf("rom %s · %d frames de boot\n\n", argv[1], boot);

    // -- El boot, grabado en vivo ---------------------------------------------
    s->record_start();
    for (int i = 0; i < boot; ++i) { s->set_input(0, 0); s->step(); }
    s->record_stop();
    const AytherRecording take = s->take_recording();
    check(take.frame_count() > 0, "el boot se grabo");
    if (!take.frame_count()) return 1;

    // -- Dónde empieza el NIVEL ------------------------------------------------
    // Barrer desde el frame 0 no sirve: el arranque pasa por el logo y el
    // título —pantallas estáticas, sin line-scroll— y la transición al level
    // dispara el corte de escena de #161, que CONGELA el stitch. Todo lo que se
    // capturaba entonces era el título, y las has_bands quedaban del otro lado del
    // congelamiento. Un autor en el Lab tampoco haría eso: autora la Panorámica
    // de un level, no de la transición.
    //
    // El level se reconoce por su propia señal: `scene_dirty` bit2 = hscroll por
    // línea con variación real, que es la definición operativa de «acá hay
    // has_bands».
    uint32_t level = 0;
    for (uint32_t f = 0; f < take.frame_count(); ++f) {
        const FrameView* fv = s->replay_seek(take, f);
        if (fv && (fv->scene_dirty & 4)) { level = f; break; }
    }
    check(level > 0, "se encontro un frame con line-scroll (el level)");
    if (!level) return 1;
    // Unos frames de margen: el bit2 puede prender en el wipe de entrada, y
    // capturar desde ahí mete el arte de la transición en la tira.
    const uint32_t margen = 60;
    std::printf("  primer frame con has_bands: f%u (se captura desde f%u)\n",
                level, level + margen);

    // -- La Panorámica del plano B, con el protocolo del Lab -------------------
    const uint8_t plane = 1;   // el parallax de Sonic vive en B (medido)
    const uint32_t f0 = level + margen, f1 = take.frame_count() - 1;
    s->replay_seek(take, f0);
    s->bg_capture(true);
    for (uint32_t f = f0; f <= f1; ++f) s->replay_seek(take, f);
    s->bg_capture(false);

    const std::string png = root + "/build/pano_band_smoke.png";
    auto w = s->export_background_plane(plane, png);
    check((bool)w, "la lamina del plano B se exporta");
    if (!w) return 1;

    // #349: los bounds del STITCHER son la autoridad — son los del PNG. Los
    // extents de bg_cells() pueden diferir en una fila/columna y ese desfase
    // estira y corre el asset al dibujar.
    int32_t bb[4];
    if (!s->bg_bounds(plane, bb)) { std::fprintf(stderr, "[FAIL] sin celdas\n"); return 1; }
    const auto cells = s->bg_cells(plane);
    s->define_panorama(0xBA9DA5u, plane, bb[0], bb[1],
                       (uint16_t)(bb[2] - bb[0] + 1), (uint16_t)(bb[3] - bb[1] + 1),
                       cells.data(), (uint32_t)cells.size(), png);
    std::printf("  lamina %ux%u celdas · %zu celdas-hash\n\n",
                bb[2] - bb[0] + 1, bb[3] - bb[1] + 1, cells.size());

    // -- El barrido: qué emite la Panorámica frame a frame ---------------------
    // Se recorre la toma entera buscando el frame que MÁS has_bands distintas
    // produce. Elegir un frame fijo haría que el oráculo dependiera de dónde
    // cae el boot, que cambia con la ROM y con el largo del arranque.
    uint32_t best_frame = 0, best_quad_count = 0, best_camera_count = 0;
    uint32_t anchored_frame_count = 0, frames_multi = 0;
    // Los dos fenómenos por separado, porque pueden no coincidir NUNCA y ése
    // sería el diagnóstico: `scene_dirty` bit2 = hscroll por línea con
    // variación real en el span visible, que es la definición operativa de «acá
    // hay has_bands». Si ningún frame tiene las dos cosas, el voto por banda no
    // llega a correr y decirlo es más útil que un «0 quads» sin explicación.
    uint32_t band_frame_count = 0, both_plane_frame_count = 0;
    uint32_t max_band_cell_count = 0, max_band_vote_count = 0;
    for (uint32_t f = f0; f <= f1; ++f) {
        const FrameView* fv = s->replay_seek(take, f);
        if (!fv) continue;
        const bool has_bands = (fv->scene_dirty & 4) != 0;
        if (has_bands) {
            ++band_frame_count;
            // Cuánto le FALTÓ anclar en un frame con has_bands. Si nunca votó
            // nadie, el problema es la tira (no reconoce el level); si votaron
            // pero no alcanzó, es el umbral. Son dos arreglos distintos y sin
            // este dato la conclusión sería una corazonada.
            if (fv->panorama_cells > max_band_cell_count) {
                max_band_cell_count = fv->panorama_cells;
                max_band_vote_count = fv->panorama_votes;
            }
        }
        if (!fv->panorama_valid || !fv->panorama_sub_count) continue;
        ++anchored_frame_count;
        if (has_bands) ++both_plane_frame_count;
        if (fv->panorama_sub_count > 1) ++frames_multi;
        // Cámaras DISTINTAS, no quads: una tira recortada por el borde también
        // da varios quads con la MISMA cámara, y eso no es parallax. La cámara
        // de un quad se recupera de su rect y su UV — `u0 * rw` es el píxel de
        // la tira que arranca, y `screen_x` dónde se ve.
        std::set<int32_t> cameras;
        for (uint32_t q = 0; q < fv->panorama_sub_count; ++q) {
            const AytherSpriteSub& sq = fv->panorama_subs[q];
            cameras.insert((int32_t)(sq.u0 * 100000.f) - sq.screen_x * 1000);
        }
        if (cameras.size() > best_camera_count ||
            (cameras.size() == best_camera_count && fv->panorama_sub_count > best_quad_count)) {
            best_camera_count = (uint32_t)cameras.size();
            best_quad_count   = fv->panorama_sub_count;
            best_frame       = f;
        }
    }

    std::printf("  %u frames con line-scroll real (scene_dirty bit2)\n",
                band_frame_count);
    std::printf("  %u frames con la tira anclada · %u de ellos CON has_bands\n",
                anchored_frame_count, both_plane_frame_count);
    std::printf("  %u frames con mas de un quad\n", frames_multi);
    std::printf("  en los frames CON has_bands, lo mas cerca que estuvo de anclar:"
                " %u votos de %u celdas\n", max_band_vote_count, max_band_cell_count);
    std::printf("  mejor frame: f%u — %u quads, %u cameras distintas\n\n",
                best_frame, best_quad_count, best_camera_count);

    // CONTROL DE NO VACUIDAD. Sin frames anclados el resto no mide nada: los
    // dos chequeos de abajo darían «0 no es > 1» y el oráculo pasaría a rojo
    // por la razón equivocada, o peor, un `>= 0` pasaría a verde.
    check(anchored_frame_count > 0, "CONTROL: hay frames con la tira anclada que medir");
    if (!anchored_frame_count) return 1;

    check(best_quad_count > 1, "la tira se emite en VARIOS quads (has_bands)");
    check(best_camera_count > 1,
          "y con CAMARAS DISTINTAS — es parallax, no un recorte de borde");

    // EL DIAGNÓSTICO, cuando falla. Sin esto el rojo dice «0 quads» y no dice
    // POR QUÉ, que es la diferencia entre «el voto por banda está mal» y «el
    // voto por banda nunca llega a correr». Son arreglos en archivos distintos.
    if (g_fails && !both_plane_frame_count) {
        std::printf(
            "\n  DIAGNOSTICO: los dos fenomenos NO COINCIDEN en esta toma."
            "\n  La tira ancla en %u frames y hay has_bands en %u, y la interseccion es"
            "\n  CERO. Antes de buscar el defecto en el motor, revisar el BARRIDO: si"
            "\n  arranca antes del level, la transicion dispara el corte de escena"
            "\n  (#161) y CONGELA el stitch con el titulo adentro. El sintoma es una"
            "\n  lamina de %ux%u celdas —una pantalla— con %zu celdas-hash apiladas."
            "\n  Fue exactamente el primer error de este oraculo, y costo una issue"
            "\n  abierta de mas.",
            anchored_frame_count, band_frame_count,
            bb[2] - bb[0] + 1, bb[3] - bb[1] + 1, cells.size());
    }

    // -- El detalle del mejor frame, para que el numero sea auditable ---------
    {
        const FrameView* fv = s->replay_seek(take, best_frame);
        if (fv) {
            for (uint32_t q = 0; q < fv->panorama_sub_count && q < 12; ++q) {
                const AytherSpriteSub& sq = fv->panorama_subs[q];
                std::printf("    quad %2u: y %3d..%-3d  x %3d  %ux%u  u0 %.4f\n",
                            q, sq.screen_y, sq.screen_y + sq.h_px,
                            sq.screen_x, sq.w_px, sq.h_px, sq.u0);
            }
            // Las has_bands no se solapan y cubren en orden: si se solaparan, dos
            // cámaras escribirían la misma línea y la de arriba ganaría al azar.
            bool sorted_bands = true;
            for (uint32_t q = 1; q < fv->panorama_sub_count; ++q)
                if (fv->panorama_subs[q].screen_y <
                    fv->panorama_subs[q - 1].screen_y) sorted_bands = false;
            check(sorted_bands, "las has_bands salen en orden de linea, sin solaparse");
        }
    }

    std::printf("\n%d checks, %d fails\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
