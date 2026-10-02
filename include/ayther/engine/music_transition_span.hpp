#pragma once

#include <ayther/engine/music_sequence.hpp>

#include <cstdint>
#include <string>

namespace ayther::engine {

struct MusicTransitionSpan {
  std::uint64_t structural_boundary{};
  std::uint64_t audible_end{};
  SampleRegion tail_region{};
  SampleRegion replaced_region{};
  SequenceNodeId destination{};
  std::uint64_t link_duration_frames{};
  std::uint32_t sample_rate{};
  bool transition_included_in_asset{};
  bool foreign_effect{};
  bool tail_loops{};
};

struct MusicTransitionSpanDecision {
  bool valid{};
  std::uint64_t structural_boundary{};
  std::uint64_t audible_end{};
  SampleRegion replaced_region{};
  bool play_tail{};
  bool play_link{};
  std::string diagnostic;
};

[[nodiscard]] inline MusicTransitionSpanDecision
evaluate_transition_span(const MusicTransitionSpan &span) {
  MusicTransitionSpanDecision result;
  result.structural_boundary = span.structural_boundary;
  result.audible_end = span.audible_end;
  result.replaced_region = span.replaced_region;
  if (!span.destination || span.sample_rate == 0 ||
      !span.replaced_region.valid() ||
      span.audible_end < span.structural_boundary) {
    result.diagnostic = "invalid_transition_span";
    return result;
  }
  if (span.link_duration_frames >
      static_cast<std::uint64_t>(span.sample_rate) * 10ULL) {
    result.diagnostic = "link_duration_limit";
    return result;
  }
  if (span.tail_loops) {
    result.diagnostic = "indefinite_tail_forbidden";
    return result;
  }
  result.valid = true;
  result.play_tail = span.tail_region.valid() && !span.foreign_effect;
  result.play_link = span.link_duration_frames != 0 &&
                     !span.transition_included_in_asset;
  return result;
}

} // namespace ayther::engine
