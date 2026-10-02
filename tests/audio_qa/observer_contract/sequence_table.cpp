#include <ayther/audio_seq_anchor.h>

#include <array>
#include <iostream>

namespace {
struct Trace {
  std::array<std::uint64_t, 4> signatures{};
  std::array<std::size_t, 4> sources{};
  std::array<std::uint32_t, 2> frames{};
  std::size_t input_count = 0;
  std::size_t frame_count = 0;
  std::size_t visits = 0;
  bool valid = true;
  void begin(const ayther::SeqAnchorFrameView &view) noexcept {
    if (frame_count >= frames.size() || view.signatures.size() != 2 ||
        view.source_indices.size() != view.signatures.size() ||
        view.substitutions.size() != 1 || view.states.size() != 1) {
      valid = false;
      return;
    }
    frames[frame_count] = view.frame;
    if ((frame_count == 0 && view.states[0].open) ||
        (frame_count == 1 &&
         (!view.states[0].open || view.states[0].win_start != 0 ||
          view.states[0].win_end != 5)))
      valid = false;
    ++frame_count;
    for (std::size_t i = 0; i < view.signatures.size(); ++i) {
      if (input_count >= signatures.size()) {
        valid = false;
        return;
      }
      signatures[input_count] = view.signatures[i];
      sources[input_count++] = view.source_indices[i];
    }
  }
  void visit(const ayther::SeqAnchorCandidateView &view) noexcept {
    if (frame_count == 0 || view.frame != frames[frame_count - 1] ||
        view.sub_index != 0)
      valid = false;
    ++visits;
  }
};

bool stable_table() {
  const std::array<std::uint64_t, 4> signatures{7, 8, 7, 7};
  const std::array<std::uint32_t, 4> starts{10, 0, 0, 10};
  const std::vector<ayther::SeqAnchorSub> subs{
      {10, 7, 5, 5, true, false, {7, 8}, {7, 8}}};
  std::size_t plain_sig_reads = 0, plain_frame_reads = 0;
  const auto plain = ayther::seq_anchor_table(
      signatures.size(),
      [&](std::size_t i) {
        ++plain_sig_reads;
        return signatures[i];
      },
      [&](std::size_t i) {
        ++plain_frame_reads;
        return starts[i];
      },
      subs);
  std::size_t observed_sig_reads = 0, observed_frame_reads = 0;
  Trace trace;
  const auto observed = ayther::seq_anchor_table_observed(
      signatures.size(),
      [&](std::size_t i) {
        ++observed_sig_reads;
        return signatures[i];
      },
      [&](std::size_t i) {
        ++observed_frame_reads;
        return starts[i];
      },
      subs,
      [&](const ayther::SeqAnchorFrameView &view) noexcept {
        trace.begin(view);
      },
      [&](const ayther::SeqAnchorCandidateView &view) noexcept {
        trace.visit(view);
      });
  return observed == plain &&
         observed.at(10) == std::vector<std::uint32_t>{0, 10} && trace.valid &&
         trace.frame_count == 2 && trace.visits == 4 &&
         trace.input_count == 4 &&
         trace.frames == std::array<std::uint32_t, 2>{0, 10} &&
         trace.sources == std::array<std::size_t, 4>{1, 2, 0, 3} &&
         trace.signatures == std::array<std::uint64_t, 4>{8, 7, 7, 7} &&
         plain_sig_reads == 4 && observed_sig_reads == plain_sig_reads &&
         observed_frame_reads == plain_frame_reads;
}

bool empty_and_dropped() {
  const std::vector<ayther::SeqAnchorSub> subs{
      {10, 7, 1, 1, true, false, {7}, {7}}};
  std::size_t begins = 0, visits = 0, reads = 0;
  const auto signature = [&](std::size_t) {
    ++reads;
    return std::uint64_t{7};
  };
  const auto frame = [](std::size_t i) {
    return static_cast<std::uint32_t>(i);
  };
  const auto begin = [&](const ayther::SeqAnchorFrameView &) noexcept {
    ++begins;
  };
  const auto visit = [&](const ayther::SeqAnchorCandidateView &) noexcept {
    ++visits;
  };
  const auto empty = ayther::seq_anchor_table_observed(0, signature, frame,
                                                       subs, begin, visit);
  if (empty.size() != 1 || !empty.at(10).empty() || begins || visits || reads)
    return false;
  // The receiver retains no views; all 128 frames still pass through one
  // traversal.
  const auto result = ayther::seq_anchor_table_observed(128, signature, frame,
                                                        subs, begin, visit);
  if (begins != 128 || visits != 128 || reads != 128 ||
      result.at(10).size() != 128)
    return false;
  return result == ayther::seq_anchor_table(128, signature, frame, subs);
}

struct CopyOnlyReader {
  CopyOnlyReader() = default;
  CopyOnlyReader(const CopyOnlyReader &) = default;
  CopyOnlyReader &operator=(const CopyOnlyReader &) = default;
  CopyOnlyReader(CopyOnlyReader &&) = delete;
  CopyOnlyReader &operator=(CopyOnlyReader &&) = delete;
  ~CopyOnlyReader() = default;
  std::uint64_t operator()(std::size_t) const noexcept { return 7; }
};
bool original_reader_contract() {
  const CopyOnlyReader reader;
  const std::vector<ayther::SeqAnchorSub> subs{
      {10, 7, 1, 1, true, false, {7}, {7}}};
  const auto result =
      ayther::seq_anchor_table(1, reader, [](std::size_t) { return 0u; }, subs);
  return result.at(10) == std::vector<std::uint32_t>{0};
}
} // namespace

int main() {
  try {
    if (!stable_table() || !empty_and_dropped() ||
        !original_reader_contract()) {
      std::cerr << "Observed sequence table failed\n";
      return 1;
    }
    std::cout
        << "Stable sequence table provenance and equal read counts verified\n";
  } catch (...) {
    return 2;
  }
}
