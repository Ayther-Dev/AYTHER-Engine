// Spec 002, R6 (RF-9.1, RF-9.2): a replacement hides the originals it claims
// only while its texture is resident. Pure: the compose asks it per claimed
// element with the state of that replacement's texture.
#pragma once

#include <cstdint>

namespace ayther::session {

/// State of a replacement texture in the renderer's cache.
enum class Residency : std::uint8_t {
  not_requested, ///< nothing asked for it yet
  pending,       ///< decoding or uploading
  ready,         ///< resident: it can be drawn this frame
  failed,        ///< the asset could not be decoded (missing_asset)
};

/// What the compose draws for an element that a replacement claims.
enum class ClaimedDraw : std::uint8_t {
  replacement,      ///< the replacement, instead of the original
  original,         ///< the original: the texture is not resident yet
  original_missing, ///< the original, and the asset is reported missing
};

/// The rule «claim only with the texture resident»: anything short of a
/// resident texture leaves the original drawn, so no frame loses the element.
constexpr ClaimedDraw claimed_draw(Residency texture) noexcept {
  switch (texture) {
  case Residency::ready:
    return ClaimedDraw::replacement;
  case Residency::failed:
    return ClaimedDraw::original_missing;
  case Residency::not_requested:
  case Residency::pending:
    break;
  }
  return ClaimedDraw::original;
}

} // namespace ayther::session
