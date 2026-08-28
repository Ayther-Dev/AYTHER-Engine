#pragma once
// ---------------------------------------------------------------------------
// pano_bands.h — la cámara de una Panorámica, VOTADA POR BANDA ().
//
// EL PROBLEMA. La Panorámica modela una tira rígida con UNA cámara: cada celda
// visible vota `cam_px = lx*8 - screen_x` y gana la moda. Cuando el plano tiene
// line-scroll —bandas que se desplazan a distinto ritmo dentro de la MISMA
// capa del VDP— no existe una posición que las explique a todas: las celdas de
// la banda rápida votan contra las del fondo. En el mejor caso gana la moda y
// la banda minoritaria queda mal ubicada; en el peor el voto se parte y el
// anclaje no fija.
//
// QUE EL CASO EXISTE está medido, no supuesto (2026-08-24, hscroll_bands_probe):
//
//   Golden Axe   3 tomas, 40.854 frames   reg $B modo 0   0 bandas
//   Ecco         1.800 frames             reg $B modo 0   0 bandas
//   Aladdin      1.800 frames             reg $B modo 0   0 bandas
//   Sonic 3 & K  1.800 frames             tabla por línea en 1.766
//                                         plano A: 1 banda · plano B: 37
//
// Golden Axe NO es el corpus de esta feature —sus nubes de título se
// resolvieron como dos Acetatos en paralaje, —; Sonic 3 & Knuckles sí.
//
// LA FORMA DE LA SOLUCION. Con 37 bandas, declarar una deriva por tira (la
// dirección 2 de la issue) no alcanza: serían 37 velocidades que el autor
// tendría que mantener a mano. Se vota POR BANDA, que es lo que hace el
// hardware.
//
// Este archivo es sólo el VOTO: agrupa y decide, no lee VRAM, no toca Vulkan y
// no sabe qué es una Panorámica. Igual que `widescreen.h`, se puede medir sin
// GPU y sin ROM — que es como se encontró el bug de bandas de EM-8.0.
// ---------------------------------------------------------------------------
#include <cstddef>
#include <cstdint>
#include <vector>

#include "parallax_bands.h"   // hscroll_of_line: la misma lectura que EM-8.0

namespace ayther {

/// Un voto: una celda visible dice dónde estaría la cámara si ella manda.
struct PanoVote {
    int32_t  screen_y;   ///< dónde se ve (px) — decide a qué banda pertenece
    int32_t  cam_x;      ///< lx*8 - screen_x
    int32_t  cam_y;      ///< ly*8 - screen_y
};

/// La cámara que ganó en una banda de líneas.
struct BandCam {
    int32_t  y0 = 0, y1 = 0;   ///< rango de líneas [y0, y1) de la banda
    int32_t  cam_x = 0, cam_y = 0;
    uint32_t votes = 0;        ///< cuántos votos sacó la ganadora
    uint32_t total = 0;        ///< cuántos votó la banda en total
    /// Sin votos la banda NO tiene cámara: `total == 0` es distinto de «la
    /// cámara es (0,0)». Confundirlos dibuja la tira en el origen, que es un
    /// defecto visible y difícil de atribuir.
    bool decided() const { return total != 0; }
    /// Qué tan sólida fue la decisión: 1.0 = todas las celdas coincidieron.
    /// Por debajo de ~0.5 la banda está mezclando dos scrolls y conviene
    /// mirarla antes que confiar en ella.
    float confidence() const {
        return total ? float(votes) / float(total) : 0.0f;
    }
};

/// Vota una cámara por banda.
///
/// `bands` son los cortes de línea, en orden ascendente y sin solaparse: la
/// banda i cubre [bands[i], bands[i+1]). Salen de la tabla Hscroll del VDP —
/// las corridas contiguas de líneas con el mismo valor de scroll, que es lo
/// que ya cuenta `hscroll_bands_probe` y lo que `parallax_bands.h` modela para
/// la cámara de EM-8.0.
///
/// Un `bands` de un solo corte (o vacío) devuelve UNA banda que cubre todo, y
/// entonces esto se comporta EXACTAMENTE como el voto de hoy — que es lo que
/// hace que el cambio sea seguro para los 40.854 frames de Golden Axe medidos
/// sin una sola banda.
///
/// El desempate es DETERMINISTA y por eso está acá y no en el caller: con `>` a
/// secas el ganador de un empate depende del orden de iteración de un hash, y
/// eso hace que dos corridas idénticas difieran (la trampa que ya costó en el
/// voto de la Panorámica). Gana el que más votos tiene; a igualdad, el de
/// `cam_x` menor, y a igualdad de eso el de `cam_y` menor.
inline std::vector<BandCam> pano_vote_by_band(const PanoVote* votes, size_t n,
                                              const int32_t* bands, size_t nbands,
                                              int32_t screen_h) {
    std::vector<BandCam> out;
    // Sin cortes útiles: una sola banda que cubre la pantalla (el modelo viejo).
    if (!bands || nbands < 2) {
        out.push_back(BandCam{0, screen_h, 0, 0, 0, 0});
    } else {
        out.reserve(nbands - 1);
        for (size_t i = 0; i + 1 < nbands; ++i)
            out.push_back(BandCam{bands[i], bands[i + 1], 0, 0, 0, 0});
    }
    if (!votes || n == 0) return out;

    // Un tally por banda. Son pocas (37 en el peor caso medido) y pocos votos
    // por banda, así que un vector lineal gana al hash: menos asignaciones y,
    // sobre todo, orden de recorrido ESTABLE para el desempate.
    struct Cnt { int32_t cx, cy; uint32_t k; };
    std::vector<std::vector<Cnt>> tally(out.size());

    for (size_t v = 0; v < n; ++v) {
        const PanoVote& pv = votes[v];
        size_t b = out.size();   // fuera de toda banda = se descarta
        for (size_t i = 0; i < out.size(); ++i)
            if (pv.screen_y >= out[i].y0 && pv.screen_y < out[i].y1) { b = i; break; }
        if (b == out.size()) continue;
        ++out[b].total;
        bool found = false;
        for (Cnt& c : tally[b])
            if (c.cx == pv.cam_x && c.cy == pv.cam_y) { ++c.k; found = true; break; }
        if (!found) tally[b].push_back(Cnt{pv.cam_x, pv.cam_y, 1});
    }

    for (size_t i = 0; i < out.size(); ++i) {
        const Cnt* best = nullptr;
        for (const Cnt& c : tally[i]) {
            if (!best || c.k > best->k ||
                (c.k == best->k && (c.cx < best->cx ||
                                    (c.cx == best->cx && c.cy < best->cy))))
                best = &c;
        }
        if (best) { out[i].cam_x = best->cx; out[i].cam_y = best->cy; out[i].votes = best->k; }
    }
    return out;
}

/// Los CORTES de banda de un plano, leyendo la tabla Hscroll del VDP.
///
/// Devuelve los límites en LÍNEAS de pantalla para `pano_vote_by_band`: la
/// banda i cubre [out[i], out[i+1]). Siempre arranca en 0 y termina en
/// `rows * 8`, así que un plano sin parallax devuelve exactamente {0, alto} —
/// una sola banda, y el voto se comporta como el de siempre.
///
/// `band_count()` (parallax_bands.h) responde CUÁNTAS hay; esto responde DÓNDE
/// están, que es lo que el voto necesita. Comparten la lectura de la tabla y
/// el muestreo cada 8 líneas: el VDP resuelve el scroll por celda o por línea
/// según el reg $0B, y muestrear más fino no agrega bandas — agrega ruido.
///
/// `mask == 0` (reg $0B modo 0, scroll entero) devuelve una sola banda sin
/// leer nada: es el 100 % del corpus medido salvo Sonic 3 & Knuckles.
template <typename ReadU32>
inline std::vector<int32_t> pano_band_edges(const ReadU32& read_u32,
                                            uint32_t base, uint32_t mask,
                                            uint8_t plane, int rows) {
    std::vector<int32_t> out;
    const int32_t h = static_cast<int32_t>(rows > 0 ? rows * 8 : 0);
    out.push_back(0);
    if (mask == 0 || rows <= 0) { out.push_back(h); return out; }
    int last = hscroll_of_line(read_u32, base, mask, plane, 0);
    for (int r = 1; r < rows; ++r) {
        const int v = hscroll_of_line(read_u32, base, mask, plane, r * 8);
        if (v != last) { out.push_back(static_cast<int32_t>(r * 8)); last = v; }
    }
    out.push_back(h);
    return out;
}

}  // namespace ayther
