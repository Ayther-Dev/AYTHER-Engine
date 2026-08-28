#pragma once
// ---------------------------------------------------------------------------
// cram_palette.h — la CRAM de la Mega Drive, leída ( EM-9.4).
//
// Las cuatro líneas de paleta del VDP: 64 colores de 9 bits que deciden de qué
// color se ve cada índice de cada tile. Todo lo demás del pipeline —el hash de
// tile de plano, la firma de variante por paleta (), el tinte— se apoya en
// esto, y hasta ahora la conversión vivía suelta en tres lugares distintos de
// `ayther_session.cpp`.
//
// # El formato: EMPAQUETADO, no el del bus
//
// La CRAM que publica el fork viene EMPAQUETADA —R en los bits 0-2, G en 3-5,
// B en 6-8— y **no** en el formato del bus de la Genesis, que deja huecos
// (R=1-3, G=5-7, B=9-11). Confundirlos da colores que parecen razonables:
// todo sale a la mitad de intensidad y desplazado de tono, que es peor que
// salir mal del todo — nadie lo mira dos veces.
//
// Verificado contra el juego: blanco = 0x1FF, azul = 0x1E3 → R3 G4 B7.
//
// # Los 3 bits a 8: NO es `x << 5`
//
// Un componente de 3 bits que se lleva a 8 con un corrimiento nunca llega a
// 255: el blanco máximo daría 224 y toda la imagen quedaría lavada. La
// expansión correcta repite el patrón de bits, que es lo que hace que 7 → 255 y
// 0 → 0 con los intermedios repartidos parejo.
// ---------------------------------------------------------------------------
#include <cstdint>

namespace ayther {

/// Un componente de 3 bits a 8, repitiendo el patrón: `x*255/7`, que para
/// estos ocho valores es exacto y sin división en tiempo de ejecución.
inline constexpr uint8_t cram_c8(uint8_t x3) {
    const uint8_t v = static_cast<uint8_t>(x3 & 7);
    return static_cast<uint8_t>((v << 5) | (v << 2) | (v >> 1));
}

/// Un color de la CRAM, en RGB de 8 bits por canal.
struct CramColor { uint8_t r, g, b; };

/// El color `index` (0-63) de la CRAM empaquetada. Fuera de rango o sin datos
/// devuelve negro — un color de fondo es un resultado legítimo para «no sé»,
/// y devolver magenta convertiría cada lectura de más en un falso positivo
/// visual.
inline CramColor cram_color(const uint8_t* cram, size_t size, uint32_t index) {
    const size_t e = static_cast<size_t>(index) * 2;
    if (!cram || index >= 64 || e + 1 >= size) return { 0, 0, 0 };
    const uint16_t v = static_cast<uint16_t>(cram[e] | (cram[e + 1] << 8));
    return { cram_c8(static_cast<uint8_t>(v & 7)),
             cram_c8(static_cast<uint8_t>((v >> 3) & 7)),
             cram_c8(static_cast<uint8_t>((v >> 6) & 7)) };
}

/// El color `entry` (0-15) de la línea `line` (0-3). Es la forma en que se
/// piensa una paleta —«el índice 3 de la línea 1»— y evita que cada consumidor
/// haga la multiplicación por su cuenta.
inline CramColor cram_color_at(const uint8_t* cram, size_t size,
                               uint8_t line, uint8_t entry) {
    return cram_color(cram, size, (line & 3u) * 16u + (entry & 15u));
}

/// La firma de una LÍNEA de paleta: FNV-1a de sus 16 words tal como están en
/// CRAM (, «variante por contenido de paleta»).
///
/// Sirve para lo que un visor necesita y una comparación visual no puede: decir
/// si dos momentos del juego tienen la MISMA paleta. Un ciclo de día/noche
/// cambia esta firma aunque en pantalla el cambio sea de un tono.
inline uint64_t cram_line_signature(const uint8_t* cram, size_t size, uint8_t line) {
    uint64_t h = 0x1465'0FB0'739D'0383ull;   // el seed de AYTHER (pack-identities §0)
    for (uint32_t e = 0; e < 16; ++e) {
        const size_t off = (static_cast<size_t>(line & 3u) * 16u + e) * 2u;
        const uint8_t lo = (cram && off     < size) ? cram[off]     : 0;
        const uint8_t hi = (cram && off + 1 < size) ? cram[off + 1] : 0;
        h = (h ^ lo) * 0x1000'0001'B3ull;
        h = (h ^ hi) * 0x1000'0001'B3ull;
    }
    return h;
}

}  // namespace ayther
