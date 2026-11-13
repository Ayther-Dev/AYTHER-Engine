// parity_bench identities (spec 008, TP-209.2, RF-10.2): the canonical
// identities a session produces inside the corpus segments, per class of the
// identity contract (RF-10.1, docs/IDENTITY_SPECIFICATION.md). Two providers
// fed the same inputs (D-7) must produce the same set of identifiers per
// segment and class; there is no translation table between them.
//
// Classes (Engine-computed, contract v1):
//   sprite           AytherSpriteOccurrence::hash
//   anim_group       AytherSpriteOccurrence::anim_group_id (0 = none, skipped)
//   tile             AytherTileOccurrence::hash
//   plane_tile       PlaneCellHit::hash (planes A, B and Window)
//   palette_variant  ayther_palette_signature of a CRAM line (16 slots) once it
//                    has been stable for 30 frames (the runtime latch)
//   audio_fm         AytherAudioEvent::signature, chip 0 (FM and DAC)
//   audio_psg        AytherAudioEvent::signature, chip 1
//   instrument       AytherAudioEvent::instrument
// An audio event belongs to the segment of its start frame. The legacy PCM
// batch hash (FrameView::audio_occs) is not an identity of contract v1: it is
// a hash of mixed samples at the provider's rate (EXC-008).
//
// File format (text, one record per line, ids sorted):
//   parity_bench identities 1
//   frames <N>
//   segment <first> <last>
//   class <name> occurrences <n> distinct <d>
//   id <hex16>
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace ayther {
struct FrameView;
} // namespace ayther

namespace ayther::parity {

struct Segment {
  std::uint32_t first = 0;
  std::uint32_t last = 0;
};

/// "A-B[,A-B]..." with A <= B; nullopt on any malformed range.
std::optional<std::vector<Segment>> parse_segments(std::string_view text);

inline constexpr const char *kIdentityClasses[] = {
    "sprite",          "anim_group", "tile",      "plane_tile",
    "palette_variant", "audio_fm",   "audio_psg", "instrument"};

struct ClassIds {
  std::uint64_t occurrences = 0;
  std::set<std::uint64_t> ids;
};

struct SegmentIds {
  Segment range;
  std::map<std::string, ClassIds> classes; ///< every kIdentityClasses entry
};

struct Identities {
  std::uint32_t frames = 0;
  std::vector<SegmentIds> segments;
};

/// Collects identities frame by frame. Frames are numbered from 1, as in the
/// dump; a frame outside every segment only feeds the stateful detectors.
class IdentityCollector {
public:
  explicit IdentityCollector(std::vector<Segment> segments);
  ~IdentityCollector();
  IdentityCollector(const IdentityCollector &) = delete;
  IdentityCollector &operator=(const IdentityCollector &) = delete;

  /// `cram` (128 bytes, host order) stands in when the view carries no
  /// scene CRAM (scene compose off); null when the provider has none.
  void frame(std::uint32_t frame, const FrameView &view,
             const std::uint8_t *cram, std::size_t cram_size);
  /// Closes the audio events still open and returns the result.
  Identities finish();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

std::string format_identities(const Identities &ids);
/// Strict parser of format_identities; nullopt (and `error`) on bad input.
std::optional<Identities> parse_identities(std::string_view text,
                                           std::string *error);

struct ClassDiff {
  std::string name;
  std::size_t golden = 0;    ///< distinct ids in the golden set
  std::size_t candidate = 0; ///< distinct ids in the candidate set
  std::size_t missing = 0;   ///< golden ids the candidate lacks
  std::size_t extra = 0;     ///< candidate ids the golden lacks
  std::uint64_t golden_occurrences = 0;
  std::uint64_t candidate_occurrences = 0;
  std::uint64_t set_hash = 0; ///< FNV-1a 64 over the golden ids when equal
  std::vector<std::uint64_t> missing_ids, extra_ids; ///< first 8 of each
};

struct SegmentDiff {
  Segment range;
  std::vector<ClassDiff> classes;
};

struct IdentityComparison {
  bool ok = false;
  std::string error;
  std::vector<SegmentDiff> segments;
  bool equal() const;
};

IdentityComparison compare_identities(const Identities &golden,
                                      const Identities &candidate);
/// The comparison as one JSON object (hashes and counts only, D-2).
std::string comparison_json(const IdentityComparison &cmp);

} // namespace ayther::parity
