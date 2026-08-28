// ---------------------------------------------------------------------------
// pano_cover_probe — la COBERTURA de una Panorámica, medida (#565).
//
// # Qué contesta
//
// El arreglo de #565 cambió contra qué se verifica la cobertura: antes, contra
// cualquiera de los hashes que pasaron por una posición de la tira; ahora,
// contra el que la lámina DIBUJA. Eso mueve el número, y la pregunta es cuánto.
//
// El oráculo `panorama_cover` fija la REGLA con tres hashes inventados. Esto
// mide el EFECTO sobre un juego real: barre una toma, arma la Panorámica con el
// mismo protocolo que «Crear Panorámica» del Lab, y reporta por frame la
// cobertura y la limpieza de la tira.
//
// # Por qué hace falta medir y no alcanza con el test
//
// Porque el arreglo tiene un riesgo que un test unitario no puede ver: si la
// tira es ambigua en muchas posiciones, la cobertura BAJA, y si baja de más el
// anclaje deja de declararse y la Panorámica no se dibuja. Un arreglo que
// evita mostrar el tramo equivocado apagando la función no arregló nada.
//
// Los dos números que decide mirar juntos:
//
//   cobertura  — qué fracción de las celdas visibles del plano EXPLICA la tira
//                en la posición anclada.
//   limpieza   — qué fracción de las posiciones de la tira tiene UN solo hash.
//                Es la que dice cuánto puede haber cambiado el arreglo: en una
//                tira 100 % limpia, verificar contra el dibujado o contra
//                cualquiera es exactamente lo mismo.
//
// Uso:  pano_cover_probe <core> <rom> [frames] [plano 0|1]
// ---------------------------------------------------------------------------
#include "ayther_session.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("uso: pano_cover_probe <core> <rom> [frames] [plano 0|1]\n");
        return 2;
    }
    const std::string core = argv[1], rom = argv[2];
    const int   frames = argc > 3 ? std::atoi(argv[3]) : 1200;
    const uint8_t plane = (uint8_t)(argc > 4 ? std::atoi(argv[4]) : 1);

    ayther::AytherSession::Config cfg;
    cfg.core_path = core;
    cfg.rom_path  = rom;
    cfg.enable_audio = false;
    cfg.derive_core_pack = false;
    auto abierta = ayther::AytherSession::create(cfg);
    if (!abierta) {
        std::fprintf(stderr, "[FAIL] no abrio: %s\n", abierta.error.message.c_str());
        return 1;
    }
    ayther::AytherSession& s = **abierta.value;

    // -- 1. Barrer ------------------------------------------------------------
    //
    // El barrido es lo que llena el stitcher: sin él no hay lámina, y sin
    // lámina la cobertura no significa nada. Se avanza con la derecha apretada
    // para que la cámara recorra el nivel — un juego quieto produce una tira de
    // una pantalla, y sobre eso el arreglo no se puede medir.
    s.bg_capture(true);
    const uint16_t kDerecha = 1u << 7;   // RETRO_DEVICE_ID_JOYPAD_RIGHT
    const uint16_t kStart   = 1u << 3;
    for (int f = 0; f < frames; ++f) {
        // Arranque desatendido: Start en los primeros frames para pasar títulos.
        s.set_input(0, f < 240 ? (uint16_t)(f % 30 < 4 ? kStart : 0) : kDerecha);
        s.step();
    }
    const auto cells = s.bg_cells(plane);
    std::printf("=== pano_cover_probe (#565) ===\n");
    std::printf("  barrido: %d frames, plano %u\n", frames, (unsigned)plane);
    std::printf("  celdas de la lamina: %zu\n", cells.size());
    if (cells.empty()) {
        std::printf("  [--] el barrido no produjo lamina: nada que medir.\n");
        return 0;
    }

    // -- 2. Cuánto de la tira es ambiguo --------------------------------------
    //
    // Se cuenta acá y no se le pide a la sesión porque es lo que decide si esta
    // medición dice algo: en una tira 100 % limpia el arreglo no puede cambiar
    // nada, y reportar «no cambió» sobre ese caso sería un falso negativo.
    {
    size_t position_count = 0, ambiguous_count = 0;
        int64_t prev_lx = INT64_MIN, prev_ly = INT64_MIN;
        uint64_t prev_hash = 0;
        bool is_ambiguous = false;
    for (const auto& c : cells) {
            if (c.lx != prev_lx || c.ly != prev_ly) {
                if (position_count && is_ambiguous) ++ambiguous_count;
                ++position_count; is_ambiguous = false;
                prev_lx = c.lx; prev_ly = c.ly; prev_hash = c.hash;
            } else if (c.hash != prev_hash) {
                is_ambiguous = true;
            }
        }
        if (position_count && is_ambiguous) ++ambiguous_count;
        std::printf("  posiciones: %zu  ambiguas: %zu  (limpieza %.1f%%)\n",
                position_count, ambiguous_count,
                position_count ? 100.0 * (double)(position_count - ambiguous_count) /
                                     (double)position_count : 0.0);
        std::printf("  hashes por posicion: %.2f\n",
                pos ? (double)cells.size() / (double)pos : 0.0);
    }

    // -- 3. Definir la Panorámica y medir la cobertura por frame --------------
    int32_t b[4] = {};
    if (!s.bg_bounds(plane, b)) {
        std::printf("  [--] sin bounds: el barrido no dejo tira.\n");
        return 0;
    }
    s.define_panorama(1, plane, b[0], b[1],
                      (uint16_t)(b[2] - b[0] + 1), (uint16_t)(b[3] - b[1] + 1),
                      cells.data(), (uint32_t)cells.size(), "probe.png");

    uint32_t anchored_cells = 0, total = 0, cover_sum = 0, cover_min = 100, cover_max = 0;
    for (int f = 0; f < 300; ++f) {
        s.set_input(0, kDerecha);
        const ayther::FrameView& v = s.step();
        ++total;
        if (!v.panorama_valid) continue;
        ++anchored_cells;
        const uint32_t c = v.panorama_cover;
        cover_sum += c;
        if (c < cover_min) cover_min = c;
        if (c > cover_max) cover_max = c;
    }
    std::printf("\n  frames medidos: %u  anchored_cells: %u (%.0f%%)\n",
                total, anchored_cells, total ? 100.0 * anchored_cells / total : 0.0);
    if (anchored_cells) {
        std::printf("  cobertura: min %u%%  med %u%%  max %u%%\n",
                    cover_min, cover_sum / anchored_cells, cover_max);
    } else {
        // NO es lo mismo que cobertura 0: significa que el voto nunca llego al
        // piso, y ahi la cobertura ni se calcula.
        std::printf("  [--] la Panoramica nunca anclo en la ventana medida.\n");
    }
    return 0;
}
