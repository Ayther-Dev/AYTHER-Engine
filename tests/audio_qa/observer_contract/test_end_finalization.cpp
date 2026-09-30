#include "../../../tools/common/synth_rom.h"

#include <ayther/ayther_session.h>
#include <ayther/engine/audio_hd_state.hpp>

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

namespace obs = ayther::engine::audio_observation;

namespace {

std::uint64_t number(const obs::FactView &fact,
                     std::string_view name) noexcept {
  for (const auto &field : fact.fields)
    if (field.name == name)
      if (const auto *value = std::get_if<std::uint64_t>(&field.value))
        return *value;
  return UINT64_MAX;
}

std::string_view text(const obs::FactView &fact,
                      std::string_view name) noexcept {
  for (const auto &field : fact.fields)
    if (field.name == name)
      if (const auto *value = std::get_if<std::string_view>(&field.value))
        return *value;
  return {};
}

std::uint64_t occurrence(const obs::FactView &fact) noexcept {
  for (const auto &field : fact.fields)
    if (field.name == "occurrence")
      if (const auto *value = std::get_if<obs::OccurrenceId>(&field.value))
        return value->value;
  return 0;
}

struct Sink {
  std::size_t ends = 0;
  std::uint64_t expected_output = 0;
  bool valid = true;

  static void receive(void *context, const obs::FactView &fact) noexcept {
    auto &sink = *static_cast<Sink *>(context);
    if (fact.kind != "hd_voice_end")
      return;
    ++sink.ends;
    const auto voice_occurrence = occurrence(fact);
    const auto expected_source = voice_occurrence == 901 ? 3U : 7U;
    sink.valid = sink.valid &&
                 (voice_occurrence == 901 || voice_occurrence == 902) &&
                 text(fact, "reason") == "test_end" &&
                 number(fact, "source_position") == expected_source &&
                 number(fact, "source_limit") == 16 &&
                 number(fact, "output_position") == sink.expected_output &&
                 number(fact, "frame_position") == UINT64_MAX;
  }
};

obs::AudioHdVoicesState active_voices() {
  obs::AudioHdVoicesState state;
  state.pcm_assets.push_back({17, std::vector<std::int16_t>(32, 120)});
  for (const auto [occurrence, source_position] :
       {std::pair{std::uint64_t{901}, std::uint64_t{3}},
        std::pair{std::uint64_t{902}, std::uint64_t{7}}}) {
    obs::AudioHdVoiceState voice;
    voice.occurrence = occurrence;
    voice.cause_producer = 7;
    voice.cause_sequence = occurrence - 800;
    voice.business_key = 0xA0B0;
    voice.pcm_identity = 17;
    voice.source_position = source_position;
    voice.output_start = 20;
    voice.looping = true;
    voice.loop_begin = 2;
    voice.loop_end = 12;
    state.voices.push_back(voice);
  }
  state.started = 2;
  return state;
}

bool closes_only_after_freeze(const std::filesystem::path &rom_path) {
  ayther::synth::Rom rom("AYTHER QA TEST END");
  ayther::synth::program_canonical(rom, true);
  if (!rom.save(rom_path))
    return false;

  Sink sink;
  ayther::AytherSession::Config config;
  config.core_path = AYTHER_TEST_CORE_PATH;
  config.rom_path = rom_path.string();
  config.enable_audio = true;
  config.derive_core_pack = false;
  config.audio_observer = {&sink, Sink::receive, nullptr};
  auto created = ayther::AytherSession::create(config);
  if (!created)
    return false;
  auto &session = **created;

  obs::AudioHdStateHeader header;
  header.game_state_identity = "test-end-state";
  header.emulation_frame = session.audio_initial_snapshot().emulation_frame;
  header.sections = obs::kAudioHdStateRequiredSections;
  const auto state = active_voices();
  if (!session.restore_audio_hd_voices(header, "test-end-state", state)
           .restored())
    return false;

  const auto early = session.finalize_audio_hd_voices_for_test();
  if (early.accepted || early.finalized_voices != 0 ||
      session.audio_hd_voices_state() != state || sink.ends != 0)
    return false;

  const auto limit = session.freeze_audio_production();
  sink.expected_output = limit.main_sample_limit;
  const auto closed = session.finalize_audio_hd_voices_for_test();
  if (!closed.accepted || !closed.output_position_known ||
      closed.finalized_voices != 2 ||
      closed.output_position != limit.main_sample_limit ||
      closed.frame_position != limit.last_emulation_frame || !sink.valid ||
      sink.ends != 2 || !session.audio_hd_voices_state().voices.empty())
    return false;

  const auto repeated = session.finalize_audio_hd_voices_for_test();
  return repeated.accepted && repeated.output_position_known &&
         repeated.finalized_voices == 0 &&
         repeated.output_position == limit.main_sample_limit && sink.ends == 2;
}

} // namespace

int main() { // NOLINT(bugprone-exception-escape) -- Test filesystem failure is
             // fatal.
  const auto rom = std::filesystem::current_path() / "test-end.md";
  const bool valid = closes_only_after_freeze(rom);
  std::error_code ignored;
  std::filesystem::remove(rom, ignored);
  return valid ? 0 : 1;
}
