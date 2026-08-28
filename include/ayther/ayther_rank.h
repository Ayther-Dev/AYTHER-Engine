#pragma once
// ---------------------------------------------------------------------------
// ayther_rank.h — la ESCALERA de resolución: qué entidad gana cuando varias
// matchean el mismo contenido.
//
// Regla del producto (2026-07-26): el match prioriza SIEMPRE de complejidad
// MAYOR a MENOR. La entidad que gana RECLAMA su cobertura y las de menor rango
// contenidas en ella no se dibujan — no se «tapan» por orden de dibujo, ni
// siquiera se emiten.
//
// Por qué acá y no en el renderer: el orden de lanes es una consecuencia, no la
// decisión. Cuando la prioridad vive en el orden de dibujo, dos entidades
// terminan pintando la misma región y el resultado depende de quién va último
// — que es exactamente el bug que tenía el Cuadro (los quads de Utilería de esa
// pantalla se dibujaban ENCIMA del Cuadro que ya los contenía).
//
// Antes de esto no existía ninguna noción de prioridad ENTRE tipos: seis
// matchers corrían aislados, cada uno escribía su buffer del FrameView y el
// renderer los dibujaba todos. Las únicas escaleras eran INTRA-dominio, con dos
// arrays de claim incompatibles: `claimed[]` sobre las occurrences de sprite y
// `consumed[]` sobre las celdas de plano.
// ---------------------------------------------------------------------------

#include <cstdint>

namespace ayther {

/// Rango de especificidad. Mayor valor = entidad MÁS compleja = gana.
///
/// El criterio no es «cuánto ocupa» sino cuánta información hace falta para
/// afirmarla: una Cinemática exige una progresión de firmas de pantalla, un
/// Cuadro una pantalla entera, una entidad de Modo 3 una lectura de RAM, un
/// keyframe una pose EN UN ESTADO (paleta, flip, firma), una pose un conjunto
/// co-presente, y un tile suelto un único hash.
enum class MatchRank : uint8_t {
    Tile      = 0,   ///< tile de plano 1×1 / sprite suelto por hash
    Glyph     = 1,   ///< glifo: utilería + carácter de una fuente
    Prop      = 2,   ///< Utilería: conjunto co-presente con offsets relativos
    Pose      = 3,   ///< pose: conjunto de sprites co-presentes
    Keyframe  = 4,   ///< pose EN UN ESTADO observado (paleta · flip · firma)
    Entity    = 5,   ///< Modo 3: identidad por RAM, exacta por instancia
    Panorama  = 6,   ///< Panorámica: la tira del nivel de una capa
    Picture   = 7,   ///< Cuadro: la pantalla completa de las capas elegidas
    Kinematic = 8,   ///< Cinemática: una progresión ordenada de Cuadros
};

inline constexpr bool outranks(MatchRank a, MatchRank b) {
    return static_cast<uint8_t>(a) > static_cast<uint8_t>(b);
}

inline const char* rank_name(MatchRank r) {
    switch (r) {
        case MatchRank::Kinematic: return "cinematica";
        case MatchRank::Picture:   return "cuadro";
        case MatchRank::Panorama:  return "panoramica";
        case MatchRank::Entity:    return "entidad";
        case MatchRank::Keyframe:  return "keyframe";
        case MatchRank::Pose:      return "pose";
        case MatchRank::Prop:      return "utileria";
        case MatchRank::Glyph:     return "glifo";
        default:                   return "tile";
    }
}

}  // namespace ayther
