// Turning HD audio back on resumes the HD music at the emulated-clock offset.
//
// From the pause menu: «Modo HD: desactivado», play a while, «activado» —
// the original music kept playing because the live instance had no new
// key-on until its next anchor. Both ways back in must resume what is still
// alive: the assets bypass (what Play's toggle uses) and the subsystem gate
// (what the «Original» profile uses).
#include "../../../tools/common/synth_rom.h"

#include <ayther/ayther_session.h>
#include <ayther/engine/audio_hd_state.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace obs = ayther::engine::audio_observation;

namespace {

constexpr std::uint64_t kAssignedKey = 0xA0B0;

int g_fail = 0;

void check(bool ok, const char *what) {
  std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what);
  if (!ok)
    ++g_fail;
}

// One second of 16-bit stereo 44.1 kHz tone: the asset the live instance
// resumes from disk.
bool write_wav(const std::filesystem::path &path) {
  constexpr std::uint32_t rate = 44100, channels = 2, bits = 16;
  constexpr std::uint32_t frames = rate;
  std::vector<std::int16_t> pcm(static_cast<std::size_t>(frames) * channels);
  for (std::uint32_t i = 0; i < frames; ++i) {
    const auto s = static_cast<std::int16_t>(
        8000.0 * std::sin(2.0 * 3.14159265 * 440.0 * i / rate));
    pcm[static_cast<std::size_t>(i) * 2] = s;
    pcm[static_cast<std::size_t>(i) * 2 + 1] = s;
  }
  const std::uint32_t data_bytes =
      static_cast<std::uint32_t>(pcm.size() * sizeof(std::int16_t));
  const std::uint32_t byte_rate = rate * channels * bits / 8;
  const std::uint16_t block_align = channels * bits / 8;
  std::ofstream out(path, std::ios::binary);
  if (!out)
    return false;
  auto u32 = [&](std::uint32_t v) {
    out.write(reinterpret_cast<const char *>(&v), 4);
  };
  auto u16 = [&](std::uint16_t v) {
    out.write(reinterpret_cast<const char *>(&v), 2);
  };
  out.write("RIFF", 4);
  u32(36 + data_bytes);
  out.write("WAVE", 4);
  out.write("fmt ", 4);
  u32(16);
  u16(1);
  u16(static_cast<std::uint16_t>(channels));
  u32(rate);
  u32(byte_rate);
  u16(block_align);
  u16(static_cast<std::uint16_t>(bits));
  out.write("data", 4);
  u32(data_bytes);
  out.write(reinterpret_cast<const char *>(pcm.data()), data_bytes);
  return static_cast<bool>(out);
}

obs::AudioHdVoicesState one_music_voice() {
  obs::AudioHdVoicesState state;
  std::vector<std::int16_t> samples(64);
  for (std::size_t i = 0; i < samples.size(); ++i)
    samples[i] = static_cast<std::int16_t>(i * 17 - 400);
  state.pcm_assets.push_back({17, std::move(samples)});

  obs::AudioHdVoiceState voice;
  voice.occurrence = 901;
  voice.cause_producer = 7;
  voice.cause_sequence = 81;
  voice.business_key = kAssignedKey;
  voice.pcm_identity = 17;
  voice.source_position = 7;
  voice.output_start = 1000;
  voice.output_end = 1012;
  voice.output_end_known = true;
  voice.gain = 0.35F;
  voice.looping = true;
  voice.event = true;
  voice.end_frame = 100000;
  voice.cut_frame = 100005;
  voice.fade_span = 2646;
  voice.loop_begin = 4;
  voice.loop_end = 12;
  voice.late_samples = 9;
  voice.category = ayther::AudioCategory::music;
  state.voices.push_back(voice);

  state.started = 4;
  state.mixed_samples = 1234;
  state.skew_samples = 9;
  state.max_skew_samples = 9;
  return state;
}

// The logical instance behind the voice: a free loop anchored at start_frame.
obs::AudioHdRequestsPendingState one_music_instance(const std::string &asset,
                                                    std::uint64_t start_frame) {
  obs::AudioHdRequestsPendingState state;
  obs::AudioHdRequestState request;
  request.business_key = kAssignedKey;
  request.asset = asset;
  request.start_frame = start_frame;
  request.channel_mask = 1;
  request.gain = 0.35F;
  request.looping = true;
  request.sequence_substitution = false;
  request.occurrence = 901;
  state.requests.push_back(request);
  return state;
}

std::uint64_t resumed_count(const ayther::AytherSession &session) {
  std::uint64_t resumed = 0, finished = 0, offset = 0;
  session.audio_resume_stats(&resumed, &finished, &offset);
  return resumed;
}

std::uint64_t resumed_offset(const ayther::AytherSession &session) {
  std::uint64_t resumed = 0, finished = 0, offset = 0;
  session.audio_resume_stats(&resumed, &finished, &offset);
  return offset;
}

void run(const std::filesystem::path &rom_path,
         const std::filesystem::path &wav) {
  ayther::synth::Rom rom("AYTHER QA HD GATE");
  ayther::synth::program_canonical(rom, true);
  check(rom.save(rom_path), "write synthetic ROM");
  check(write_wav(wav), "write the HD music asset");

  ayther::AytherSession::Config config;
  config.core_path = AYTHER_TEST_CORE_PATH;
  config.rom_path = rom_path.string();
  config.enable_audio =
      true; // the dummy SDL driver: streams exist, nobody hears them
  config.derive_core_pack = false;
  auto created = ayther::AytherSession::create(config);
  check(static_cast<bool>(created), "create session with audio");
  if (!created)
    return;
  auto &session = **created;
  (void)session.step();
  const std::string asset = wav.string();
  session.assign_audio_event(kAssignedKey, asset.c_str());

  obs::AudioHdStateHeader header;
  header.game_state_identity = "hd-gate";
  header.emulation_frame = session.audio_initial_snapshot().emulation_frame;
  header.sections = obs::kAudioHdStateRequiredSections;
  const auto voices =
      session.restore_audio_hd_voices(header, "hd-gate", one_music_voice());
  if (!voices.restored())
    std::printf("  voices restore code %d\n", static_cast<int>(voices.code));
  const auto instance = session.restore_audio_hd_requests_pending(
      header, "hd-gate", one_music_instance(asset, header.emulation_frame));
  if (!instance.restored())
    std::printf("  requests restore code %d\n",
                static_cast<int>(instance.code));
  check(voices.restored() && instance.restored() &&
            session.audio_hd_voices_state().voices.size() == 1,
        "an HD music voice is playing with its live instance");

  // --- Play's «Modo HD: desactivado» → «activado»: the assets bypass ---------
  session.set_audio_live_bypass(true);
  check(session.audio_hd_voices_state().voices.empty(),
        "bypass on: the HD stream stops and the original plays");
  for (int i = 0; i < 30; ++i)
    (void)session.step();
  session.set_audio_live_bypass(false);
  check(resumed_count(session) == 1,
        "bypass off: the live instance is resumed, not restarted");
  check(session.audio_hd_voices_state().voices.size() == 1,
        "bypass off: the HD music is playing again");
  check(resumed_offset(session) >= 30,
        "bypass off: it resumed at the emulated-clock offset");

  // --- The «Original» profile: the subsystem gate ---------------------------
  session.set_subsystems_enabled_mask(0);
  check(session.audio_hd_voices_state().voices.empty(),
        "gate closed: the HD stream stops");
  for (int i = 0; i < 10; ++i)
    (void)session.step();
  session.set_subsystems_enabled_mask(0xFFFFFFFFu);
  check(resumed_count(session) == 2,
        "gate reopened: the live instance is resumed too");
  check(session.audio_hd_voices_state().voices.size() == 1,
        "gate reopened: the HD music is playing again");
}

} // namespace

int main() { // NOLINT(bugprone-exception-escape) -- Test filesystem failure is
             // fatal.
  const auto dir = std::filesystem::current_path();
  const auto rom = dir / "hd-gate-reopen.md";
  const auto wav = dir / "hd-gate-reopen-music.wav";
  run(rom, wav);
  std::error_code ignored;
  std::filesystem::remove(rom, ignored);
  std::filesystem::remove(wav, ignored);
  return g_fail == 0 ? 0 : 1;
}
