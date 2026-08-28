// ---------------------------------------------------------------------------
// pano_anchor_probe — anclaje de la Panorámica frame a frame, comparando el
// camino del PASO A PASO (produce de CADA frame, adv=1) contra el del PLAYBACK
// con catch-up (frames intermedios "bare" + produce sólo del visible, adv≥2 —
// el fast-forward de replay_seek). Oráculo del reporte 2026-07-31: en la demo
// de GA la transición f103–f155 (rectángulo que se expande) se ve siempre
// navegando frame a frame, pero reproduciendo a veces la tira aparece de golpe.
//
// Uso: pano_anchor_probe <core> <rom> <take.arp> <panoramas.toml> <f0> <f1> [stride]
//   stride 1 (default) = paso a paso; N>1 = playback con catch-up de N.
// Imprime una línea por frame PRODUCIDO: frame, valid, votos, cámara anclada.
// BYOR: core/rom/toma por argumentos; nada se hardcodea.
// ---------------------------------------------------------------------------
#include "ayther_session.h"
#include "ayther_recording.h"
#include "ayther_components_toml.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace ayther;

int main(int argc, char** argv) {
    if (argc < 7) {
        std::fprintf(stderr,
            "uso: pano_anchor_probe <core> <rom> <take.arp> <panoramas.toml> "
            "<f0> <f1> [stride]\n");
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

    std::ifstream pf(argv[4], std::ios::binary);
    if (!pf) { std::fprintf(stderr, "no se pudo abrir %s\n", argv[4]); return 2; }
    std::stringstream ss; ss << pf.rdbuf();
    std::vector<PackPanorama> pans;
    if (!parse_panoramas_toml(ss.str(), pans)) {
        std::fprintf(stderr, "panoramas.toml sin tiras (¿parse error arriba?)\n");
        return 2;
    }
    for (const PackPanorama& p : pans) {
        std::vector<AytherSession::PanoramaCell> cs;
        cs.reserve(p.cells.size());
        for (const auto& c : p.cells) cs.push_back({ c.hash, c.lx, c.ly });
        s.define_panorama(p.id, p.plane, p.origin_x, p.origin_y,
                          p.w_cells, p.h_cells, cs.data(), (uint32_t)cs.size(),
                          p.asset);
        std::printf("# tira 0x%016llx «%s» plano %c · %zu celdas\n",
                    (unsigned long long)p.id, p.name.c_str(),
                    p.plane ? 'B' : 'A', p.cells.size());
    }

    uint32_t f0 = (uint32_t)std::atoi(argv[5]);
    uint32_t f1 = (uint32_t)std::atoi(argv[6]);
    const uint32_t stride = argc > 7 ? (uint32_t)std::atoi(argv[7]) : 1u;
    if (f1 >= rec.frame_count()) f1 = rec.frame_count() - 1;
    if (f0 > f1 || !stride) { std::fprintf(stderr, "intervalo/stride inválido\n"); return 2; }

    // Como el Lab con HD prendido: sin esto los quads de la tira NO se emiten
    // (panorama_subs se arma sólo con hd_enabled) y el probe no vería lo que
    // ve el renderer.
    s.set_hd_enabled(true);

    // Arranque idéntico al Lab: posicionarse en f0 (camino general con
    // keyframe) y avanzar con el fast-forward — quiet=false, como el playback.
    const FrameView* fv = s.replay_seek(rec, f0);
    if (!fv) { std::fprintf(stderr, "seek falló en %u\n", f0); return 1; }
    std::printf("# stride %u · frames %u..%u\n", stride, f0, f1);
    for (uint32_t f = f0; f <= f1; f += stride) {
        fv = s.replay_seek(rec, f);
        if (!fv) { std::fprintf(stderr, "seek falló en %u\n", f); return 1; }
        // dirty: bit0 = raster mid-frame (híbrido→blit) · bit1 = dim ·
        // bit2 = h/vscroll por línea/celda variable. Cualquier bit ≠ 0 y el
        // renderer NO compone la escena → la tira no se dibuja ese frame.
        std::printf("f%-5u valid=%d votes=%-4u cam=(%d,%d) dirty=%u quads=%u tint=%u\n",
                    f, fv->panorama_valid ? 1 : 0, fv->panorama_votes,
                    fv->panorama_cam_x, fv->panorama_cam_y,
                    fv->scene_dirty, fv->panorama_sub_count,
                    fv->panorama_sub_tint ? fv->panorama_sub_tint[0] : 0u);
    }
    return 0;
}
