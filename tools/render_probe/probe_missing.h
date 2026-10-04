// render_probe (spec 002, BR-093, RF-2.11, RF-9.3): replay QA stops on an
// assigned asset that cannot be read. A replacement the renderer drew this
// frame with a failed texture (missing or undecodable) is a missing_asset:
// presenting it as a pose without assignment would hide the defect.
#pragma once

#include <ayther/ayther_core_ffi.h>
#include <ayther/engine/render_observer.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace ayther::probe {

struct MissingAsset {
  std::uint32_t replacement = 0; ///< index in the frame's sprite subs
  std::string asset;
};

/// The first replacement drawn this frame whose texture failed, if any.
inline std::optional<MissingAsset>
first_missing_asset(const engine::render_observation::DrawReport &report,
                    std::span<const AytherSpriteSub> subs) {
  namespace ro = engine::render_observation;
  for (std::size_t i = 0; i < report.replacements.size() && i < subs.size();
       ++i) {
    const ro::ReplacementDraw &row = report.replacements[i];
    if (row.draw != ro::DrawOutcome::discarded &&
        row.texture == ro::TextureState::failed)
      return MissingAsset{static_cast<std::uint32_t>(i), subs[i].asset_path};
  }
  return std::nullopt;
}

} // namespace ayther::probe
