// ---------------------------------------------------------------------------
// pano_sweep_probe — barrido de Panorámica headless (#349): el MISMO camino que
// «Crear Panorámica» del Lab (replay secuencial + captura del stitcher del
// motor + export del plano), sin UI. Oráculo del fix de la cámara de CONTENIDO:
// un juego que scrollea reescribiendo la nametable (GA sube el plano B en VRAM
// con los registros quietos) tiene que producir la tira COMPLETA — la
// composición a mano del artista (graphics/_debug/planoB.png) es la referencia.
//
// Uso: pano_sweep_probe <core.dll> <rom> <take.arp> <f0> <f1> <plane 0|1> <out.png>
// BYOR: core/rom/toma por argumentos; nada se hardcodea.
// ---------------------------------------------------------------------------
#include "ayther_session.h"
#include "ayther_recording.h"

// La TU de stb_image_write vive en el Lab; un tool headless trae la suya
// (export_background_plane la necesita para escribir el PNG).
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace ayther;

int main(int argc, char** argv) {
    if (argc < 8) {
        std::fprintf(stderr,
            "uso: pano_sweep_probe <core> <rom> <take.arp> <f0> <f1> "
            "<plane 0|1> <out.png>\n");
        return 2;
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

    auto rec_opt = AytherRecording::load(argv[3]);
    if (!rec_opt) { std::fprintf(stderr, "no se pudo cargar %s\n", argv[3]); return 2; }
    const AytherRecording rec = std::move(*rec_opt);

    uint32_t f0 = (uint32_t)std::atoi(argv[4]);
    uint32_t f1 = (uint32_t)std::atoi(argv[5]);
    const uint8_t plane = (uint8_t)(std::atoi(argv[6]) & 1);
    const char*   out   = argv[7];
    if (f1 >= rec.frame_count()) f1 = rec.frame_count() - 1;
    if (f0 > f1) { std::fprintf(stderr, "intervalo vacío\n"); return 2; }

    // Mismo protocolo que labio::sweep_panorama: posicionarse, capturar
    // SECUENCIAL (el stitcher acumula frame a frame), exportar.
    s.replay_seek(rec, f0);
    s.bg_capture(true);
    int frames = 0;
    const bool dump_vsc = std::getenv("PANO_PROBE_VSC") != nullptr;
    for (uint32_t f = f0; f <= f1; ++f) {
        const FrameView* fv = s.replay_seek(rec, f);
        if (!fv) { std::fprintf(stderr, "seek falló en %u\n", f); return 1; }
        if (dump_vsc && (f % 5 == 0))
            std::printf("f%-5u hscA=%-4d hscB=%-4d vscA=%-4d vscB=%-4d\n", f,
                        fv->plane_hscroll[0], fv->plane_hscroll[1],
                        fv->plane_vscroll[0], fv->plane_vscroll[1]);
        ++frames;
    }
    s.bg_capture(false);

    const auto cells = s.bg_cells(plane);
    int32_t lx0 = INT32_MAX, ly0 = INT32_MAX, lx1 = INT32_MIN, ly1 = INT32_MIN;
    for (const auto& c : cells) {
        if (c.lx < lx0) lx0 = c.lx; if (c.lx > lx1) lx1 = c.lx;
        if (c.ly < ly0) ly0 = c.ly; if (c.ly > ly1) ly1 = c.ly;
    }
    std::printf("barridos %d frames · plano %c · %zu celdas-hash · "
                "extents-hash (%d,%d)-(%d,%d) = %dx%d celdas\n",
                frames, plane ? 'B' : 'A', cells.size(),
                lx0, ly0, lx1, ly1,
                cells.empty() ? 0 : lx1 - lx0 + 1,
                cells.empty() ? 0 : ly1 - ly0 + 1);
    // #349: los bounds del STITCHER son la autoridad (el PNG y la def salen de
    // acá); si difieren de los extents de hashes, eso ANTES corría el asset.
    int32_t bb[4];
    if (s.bg_bounds(plane, bb))
        std::printf("bounds-stitcher (%d,%d)-(%d,%d) = %dx%d celdas (autoridad def+PNG)\n",
                    bb[0], bb[1], bb[2], bb[3],
                    bb[2] - bb[0] + 1, bb[3] - bb[1] + 1);

    auto w = s.export_background_plane(plane, out);
    if (!w) { std::fprintf(stderr, "export: %s\n", w.error.message.c_str()); return 1; }
    std::printf("PNG: %s\n", out);
    return 0;
}
