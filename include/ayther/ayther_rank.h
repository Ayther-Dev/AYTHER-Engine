#pragma once
// ---------------------------------------------------------------------------
// ayther_rank.h — the resolution LADDER: which entity wins when several match
// the same content.
//
// Product rule (2026-07-26): matching ALWAYS prioritises from HIGHER to LOWER
// complexity. The winning entity CLAIMS its coverage, and the lower-ranked
// entities contained within it are not drawn — they are not covered up by draw
// order, they are not even emitted.
//
// Why here and not in the renderer: lane order is a consequence, not the
// decision. When priority lives in the draw order, two entities end up painting
// the same region and the result depends on which one goes last — which is
// exactly the bug the Picture had (the Prop quads of that screen were drawn ON
// TOP OF the Picture that already contained them).
//
// Before this there was no notion of priority BETWEEN types: six matchers ran
// in isolation, each one wrote its own FrameView buffer and the renderer drew
// them all. The only ladders were INTRA-domain, with two incompatible claim
// arrays: `claimed[]` over sprite occurrences and `consumed[]` over plane
// cells.
// ---------------------------------------------------------------------------

#include <cstdint>

namespace ayther {

/// Specificity rank. Higher value = MORE complex entity = wins.
///
/// The criterion is not how much area it covers but how much information is
/// needed to assert it: a Kinematic requires a progression of screen
/// signatures, a Picture a whole screen, a Mode 3 entity a RAM read, a keyframe
/// a pose IN ONE STATE (palette, flip, signature), a pose a co-present set,
/// and a loose tile a single hash.
enum class MatchRank : uint8_t {
    Tile      = 0,   ///< 1×1 plane tile / loose sprite matched by hash
    Glyph     = 1,   ///< glyph: prop + a character of a font
    Prop      = 2,   ///< Prop: co-present set with relative offsets
    Pose      = 3,   ///< pose: set of co-present sprites
    Keyframe  = 4,   ///< pose IN ONE observed STATE (palette · flip · signature)
    Entity    = 5,   ///< Mode 3: identity by RAM, exact per instance
    Panorama  = 6,   ///< Panorama: the level-wide strip of one layer
    Picture   = 7,   ///< Picture: the complete screen of the chosen layers
    Kinematic = 8,   ///< Kinematic: an ordered progression of Pictures
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
