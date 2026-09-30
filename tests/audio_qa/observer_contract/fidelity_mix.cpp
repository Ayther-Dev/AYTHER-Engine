#include <ayther/audio_hd_mixer.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

namespace {

struct Capture {
  std::array<HdMixer::PositionSpan, 4> participants{};
  std::array<HdMixer::LifetimeEffect, 4> effects{};
  std::array<HdMixer::VoiceEnd, 3> ends{};
  std::size_t participant_count = 0;
  std::size_t effect_count = 0;
  std::size_t end_count = 0;
  bool valid = true;

  static void participant(void *context,
                          const HdMixer::PositionSpan &span) noexcept {
    auto &capture = *static_cast<Capture *>(context);
    if (capture.participant_count == capture.participants.size()) {
      capture.valid = false;
      return;
    }
    capture.participants[capture.participant_count++] = span;
  }

  static void effect(void *context,
                     const HdMixer::LifetimeEffect &effect) noexcept {
    auto &capture = *static_cast<Capture *>(context);
    if (capture.effect_count == capture.effects.size()) {
      capture.valid = false;
      return;
    }
    capture.effects[capture.effect_count++] = effect;
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

struct ExpectedParticipant {
  std::uint64_t occurrence;
  std::uint64_t key;
  std::uint64_t output_begin;
  std::uint64_t output_end;
  std::uint64_t source_begin;
  std::uint64_t source_end;
  float gain_begin;
  float gain_end;
};

std::shared_ptr<const std::vector<std::int16_t>> pcm() {
  auto result = std::make_shared<std::vector<std::int16_t>>(32);
  for (std::size_t i = 0; i < result->size(); ++i)
    (*result)[i] = static_cast<std::int16_t>(1000 + 100 * i);
  return result;
}

bool matches_participant(const HdMixer::PositionSpan &span,
                         const ExpectedParticipant &expected) {
  return span.identity.occurrence == expected.occurrence &&
         span.key == expected.key &&
         span.output_begin == expected.output_begin &&
         span.output_end == expected.output_end &&
         span.source_begin == expected.source_begin &&
         span.source_end == expected.source_end && span.source_limit == 16 &&
         span.effective_gain_begin == expected.gain_begin &&
         span.effective_gain_end == expected.gain_end && !span.muted_by_gain &&
         span.nonzero_contribution;
}

bool expected_pcm(const std::filesystem::path &path,
                  const std::array<std::int16_t, 16> &actual) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return false;
  const std::vector<char> expected{std::istreambuf_iterator<char>{input}, {}};
  if (expected.size() != actual.size() * sizeof(actual[0]))
    return false;
  return std::equal(expected.begin(), expected.end(),
                    reinterpret_cast<const char *>(actual.data()));
}

} // namespace

int main(int argc, char **argv) try {
  if (argc != 2)
    return 2;
  Capture capture;
  HdMixer mixer;
  mixer.set_position_observer(&capture, Capture::participant);
  mixer.set_effect_observer(&capture, Capture::effect);
  mixer.set_end_observer(&capture, Capture::end);
  const auto shared_pcm = pcm();
  const HdMixer::VoiceIdentity fade_identity{1, 5, 8};
  const HdMixer::VoiceIdentity cut_identity{2, 5, 9};
  if (!mixer.start(101, shared_pcm, 0, 0, 0.5F, true, true, 1, 1, 4, 0, 0,
                   &fade_identity) ||
      !mixer.start(202, shared_pcm, 0, 0, 0.25F, true, true, 1, 1, 0, 0, 0,
                   &cut_identity))
    return 3;

  std::array<std::int16_t, 16> output{};
  output.fill(100);
  mixer.mix_into(output.data(), 4, 0);
  mixer.tick_frame(2);
  if (mixer.voice_count() != 1)
    return 4;
  mixer.mix_into(output.data() + 8, 4, 4);

  if (!capture.valid || mixer.started() != 2 || mixer.voice_count() != 0 ||
      capture.participant_count != 3 || capture.effect_count != 3 ||
      capture.end_count != 2 || !expected_pcm(argv[1], output))
    return 5;

  const std::array expected_participants{
      ExpectedParticipant{2, 202, 0, 4, 0, 4, 0.25F, 0.25F},
      ExpectedParticipant{1, 101, 0, 4, 0, 4, 0.5F, 0.5F},
      ExpectedParticipant{1, 101, 4, 8, 4, 8, 0.5F, 0.125F}};
  for (std::size_t i = 0; i < expected_participants.size(); ++i)
    if (!matches_participant(capture.participants[i], expected_participants[i]))
      return 6;

  const auto &begin = capture.effects[0];
  const auto &advance = capture.effects[1];
  const auto &finish = capture.effects[2];
  if (begin.identity.occurrence != 1 || begin.key != 101 ||
      begin.kind != HdMixer::EffectKind::fade ||
      begin.stage != HdMixer::EffectStage::begin || begin.source_begin != 4 ||
      begin.source_end != 4 || !begin.output_range_known ||
      begin.output_begin != 4 || begin.output_end != 4 ||
      !begin.frame_position_known || begin.frame_position != 2 ||
      begin.remaining_begin != 4 || begin.remaining_end != 4 ||
      advance.stage != HdMixer::EffectStage::advance ||
      advance.source_begin != 4 || advance.source_end != 8 ||
      !advance.output_range_known || advance.output_begin != 4 ||
      advance.output_end != 8 || advance.remaining_begin != 4 ||
      advance.remaining_end != 0 || finish.stage != HdMixer::EffectStage::end ||
      finish.source_begin != 8 || finish.source_end != 8 ||
      !finish.output_range_known || finish.output_begin != 8 ||
      finish.output_end != 8)
    return 7;

  const auto &cut = capture.ends[0];
  const auto &fade = capture.ends[1];
  if (cut.identity.occurrence != 2 || cut.key != 202 ||
      cut.reason != HdMixer::EndReason::window_cut ||
      cut.source_position != 4 || !cut.output_position_known ||
      cut.output_position != 4 || !cut.frame_position_known ||
      cut.frame_position != 2 || fade.identity.occurrence != 1 ||
      fade.key != 101 || fade.reason != HdMixer::EndReason::fade_complete ||
      fade.source_position != 8 || !fade.output_position_known ||
      fade.output_position != 8)
    return 8;
  return 0;
} catch (...) {
  return 9;
}
