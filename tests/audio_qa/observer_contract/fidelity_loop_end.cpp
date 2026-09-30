#include <ayther/audio_hd_mixer.h>
#include <ayther/audio_live_resume.h>

#include <array>
#include <cmath>
#include <memory>
#include <vector>

namespace {

struct Capture {
  std::array<HdMixer::PositionSpan, 16> spans{};
  std::array<HdMixer::LoopCrossing, 8> loops{};
  std::array<HdMixer::VoiceEnd, 2> ends{};
  std::size_t span_count = 0;
  std::size_t loop_count = 0;
  std::size_t end_count = 0;
  bool valid = true;

  static void position(void *context,
                       const HdMixer::PositionSpan &span) noexcept {
    auto &capture = *static_cast<Capture *>(context);
    if (capture.span_count == capture.spans.size()) {
      capture.valid = false;
      return;
    }
    capture.spans[capture.span_count++] = span;
  }

  static void loop(void *context,
                   const HdMixer::LoopCrossing &crossing) noexcept {
    auto &capture = *static_cast<Capture *>(context);
    if (capture.loop_count == capture.loops.size()) {
      capture.valid = false;
      return;
    }
    capture.loops[capture.loop_count++] = crossing;
  }

  static void end(void *context, const HdMixer::VoiceEnd &end) noexcept {
    auto &capture = *static_cast<Capture *>(context);
    if (capture.end_count == capture.ends.size()) {
      capture.valid = false;
      return;
    }
    capture.ends[capture.end_count++] = end;
  }
};

std::shared_ptr<const std::vector<std::int16_t>> pcm() {
  auto result = std::make_shared<std::vector<std::int16_t>>(16);
  for (std::size_t i = 0; i < result->size(); ++i)
    (*result)[i] = static_cast<std::int16_t>(1000 + 100 * i);
  return result;
}

void observe(HdMixer &mixer, Capture &capture) {
  mixer.set_position_observer(&capture, Capture::position);
  mixer.set_loop_observer(&capture, Capture::loop);
  mixer.set_end_observer(&capture, Capture::end);
}

bool positions_match(const Capture &capture, std::size_t frames) {
  std::array<std::uint64_t, 24> positions{};
  positions.fill(UINT64_MAX);
  for (std::size_t i = 0; i < capture.span_count; ++i) {
    const auto &span = capture.spans[i];
    if (span.output_begin >= span.output_end ||
        span.source_end - span.source_begin !=
            span.output_end - span.output_begin ||
        span.output_end > frames || span.source_limit != 8 ||
        span.effective_gain_begin != 0.5F || span.effective_gain_end != 0.5F)
      return false;
    for (std::uint64_t output = span.output_begin; output < span.output_end;
         ++output)
      positions[output] = span.source_begin + output - span.output_begin;
  }
  for (std::size_t output = 0; output < frames; ++output)
    if (positions[output] != 2 + (output + 2) % 4)
      return false;
  return true;
}

bool resume_matches() {
  const auto decision =
      ayther::live_resume_decide(120, 30, 180, UINT64_MAX, true, 60.0, 3.0);
  return decision.action == ayther::LiveResumeAction::Restart &&
         std::abs(decision.offset_seconds - 1.5) < 1e-12;
}

bool loop_matches() {
  HdMixer mixer;
  Capture capture;
  observe(mixer, capture);
  const HdMixer::VoiceIdentity identity{1, 5, 8};
  if (!mixer.start(101, pcm(), 0, 4, 0.5F, true, true, UINT64_MAX, UINT64_MAX,
                   0, 2, 6, &identity))
    return false;
  std::array<std::int16_t, 48> output{};
  output.fill(100);
  constexpr std::array<std::size_t, 4> counts{8, 8, 4, 4};
  std::uint64_t boundary = 0;
  for (std::uint64_t frame = 0; frame < counts.size(); ++frame) {
    mixer.tick_frame(frame);
    mixer.mix_into(output.data() + boundary * 2, counts[frame], boundary);
    boundary += counts[frame];
  }
  if (!capture.valid || capture.loop_count != 6 || capture.end_count != 0 ||
      mixer.voice_count() != 1 || !positions_match(capture, 24))
    return false;
  for (std::size_t i = 0; i < capture.loop_count; ++i) {
    const auto &loop = capture.loops[i];
    if (loop.identity.occurrence != 1 || loop.key != 101 ||
        loop.output_position != 2 + i * 4 || loop.source_before != 6 ||
        loop.source_after != 2 || loop.loop_begin != 2 || loop.loop_end != 6 ||
        loop.source_limit != 8)
      return false;
  }
  return true;
}

bool window_matches() {
  HdMixer mixer;
  Capture capture;
  observe(mixer, capture);
  const HdMixer::VoiceIdentity identity{1, 5, 8};
  if (!mixer.start(101, pcm(), 0, 4, 0.5F, true, true, 2, UINT64_MAX, 0, 2, 6,
                   &identity))
    return false;
  std::array<std::int16_t, 48> output{};
  output.fill(100);
  constexpr std::array<std::size_t, 4> counts{8, 8, 4, 4};
  std::uint64_t boundary = 0;
  for (std::uint64_t frame = 0; frame < counts.size(); ++frame) {
    mixer.tick_frame(frame);
    mixer.mix_into(output.data() + boundary * 2, counts[frame], boundary);
    boundary += counts[frame];
  }
  if (!capture.valid || capture.loop_count != 5 || capture.end_count != 1 ||
      mixer.voice_count() != 0 || !positions_match(capture, 20))
    return false;
  const auto &end = capture.ends[0];
  return end.identity.occurrence == 1 && end.key == 101 &&
         end.reason == HdMixer::EndReason::window_end &&
         end.source_position == 4 && end.source_limit == 8 &&
         end.output_position_known && end.output_position == 20 &&
         end.frame_position_known && end.frame_position == 3;
}

bool natural_end_matches() {
  HdMixer mixer;
  Capture capture;
  observe(mixer, capture);
  const HdMixer::VoiceIdentity identity{1, 5, 8};
  if (!mixer.start(101, pcm(), 4, 0, 0.5F, false, false, UINT64_MAX, UINT64_MAX,
                   0, 0, 0, &identity))
    return false;
  std::array<std::int16_t, 32> output{};
  output.fill(100);
  mixer.mix_into(output.data(), output.size() / 2, 0);
  if (!capture.valid || capture.loop_count != 0 || capture.span_count != 1 ||
      capture.end_count != 1 || mixer.voice_count() != 0)
    return false;
  const auto &span = capture.spans[0];
  const auto &end = capture.ends[0];
  return span.output_begin == 4 && span.output_end == 12 &&
         span.source_begin == 0 && span.source_end == 8 &&
         end.identity.occurrence == 1 && end.key == 101 &&
         end.reason == HdMixer::EndReason::natural_end &&
         end.source_position == 8 && end.source_limit == 8 &&
         end.output_position_known && end.output_position == 12 &&
         !end.frame_position_known;
}

} // namespace

int main() try {
  if (!resume_matches())
    return 10;
  if (!loop_matches())
    return 11;
  if (!window_matches())
    return 12;
  if (!natural_end_matches())
    return 13;
  return 0;
} catch (...) {
  return 2;
}
