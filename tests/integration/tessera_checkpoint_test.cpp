#define NOMINMAX
#include <ayther/ayther_session.h>
#include <ayther/ayther_core_ffi.h>
#include <nlohmann/json.hpp>
#include <windows.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace native = ayther::engine;
using Json = nlohmann::json;
using Bytes = std::vector<std::uint8_t>;
using View = std::span<const std::uint8_t>;

void require(bool valid, std::string_view message) {
  if (!valid) throw std::runtime_error(std::string(message));
}
void ok(native::NativeResult result) {
  require(result.code == 0, "NATIVE_FAILURE:" + std::to_string(result.code));
}
Bytes read(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  require(stream.good(), "READ_FAILED");
  Bytes result{std::istreambuf_iterator<char>{stream},
               std::istreambuf_iterator<char>{}};
  require(result.size() <= 64ULL * 1024 * 1024, "INPUT_LIMIT");
  return result;
}
Json json(const std::filesystem::path &path) {
  const auto bytes = read(path);
  return Json::parse(bytes.begin(), bytes.end());
}
std::string digest(View value) {
  std::array<char, 65> hex{};
  require(ayther_sha256_hex(value.data(), value.size(), hex.data()) != 0,
          "SHA256_FAILED");
  return hex.data();
}
void put(Bytes &out, std::uint64_t value, unsigned count) {
  for (unsigned index = 0; index < count; ++index)
    out.push_back(static_cast<std::uint8_t>(value >> (index * 8)));
}
std::string frame_digest(std::string_view domain, const native::NativeFrame &frame,
                         bool video) {
  Bytes canonical(domain.begin(), domain.end());
  if (video) {
    for (auto item : {frame.width, frame.height, frame.video_format})
      put(canonical, item, 4);
    canonical.insert(canonical.end(), frame.pixels.begin(), frame.pixels.end());
  } else {
    put(canonical, frame.sample_rate, 4);
    put(canonical, frame.channels, 4);
    put(canonical, frame.pcm_frames, 8);
    canonical.insert(canonical.end(), frame.pcm.begin(), frame.pcm.end());
  }
  return digest(canonical);
}
std::uint16_t word(View data, std::size_t at) {
  require(at + 2 <= data.size(), "ORACLE_WORD_TRUNCATED");
  return std::uint16_t(data[at]) | std::uint16_t(data[at + 1]) << 8;
}
void oracle(native::NativeEmulation &machine, const native::NativeFrame &frame,
            bool active) {
  require(frame.width == 320 && frame.height == 224 && frame.row_bytes == 1280 &&
              frame.video_format == native::native_video_rgba8 &&
              frame.sample_rate == 48000 && frame.channels == 2 &&
              frame.pcm_frames > 0 && frame.pcm.size() == frame.pcm_frames * 4,
          "FX02_GEOMETRY_OR_PCM_INVALID");
  const auto center = (std::size_t(frame.height / 2) * frame.width +
                       frame.width / 2) * 4;
  require(frame.pixels[center] == (active ? 255 : 0) &&
              frame.pixels[center + 2] == (active ? 0 : 255),
          "FX02_VIDEO_ORACLE_FAILED");
  int minimum = 32767, maximum = -32768;
  for (std::size_t at = 0; at < frame.pcm.size(); at += 2) {
    const auto bits = word(frame.pcm, at);
    const int sample = bits < 32768 ? bits : int(bits) - 65536;
    minimum = std::min(minimum, sample);
    maximum = std::max(maximum, sample);
  }
  require(active ? maximum - minimum > 16 : minimum >= -2 && maximum <= 2,
          "FX02_AUDIO_ORACLE_FAILED");
  native::NativeSnapshot snapshot;
  ok(machine.snapshot_acquire(snapshot));
  native::NativeRegion ram, cram;
  ok(snapshot.region(0, ram));
  ok(snapshot.region(1, cram));
  require(ram.kind == 1 && ram.data.size() >= 131072 && cram.kind == 2 &&
              cram.data.size() >= 2, "FX02_REGIONS_INVALID");
  const auto marker = std::uint16_t(ram.data[65536 + 0xFFFC]) << 8 |
                      ram.data[65536 + 0xFFFD];
  require(ram.data[1] == (active ? 0 : 16) &&
              marker == (active ? 0x2468 : 0x1357) &&
              word(cram.data, 0) == (active ? 0x000E : 0x0E00),
          "FX02_MACHINE_ORACLE_FAILED");
  ok(snapshot.release());
}
std::unique_ptr<ayther::AytherSession> create(const std::filesystem::path &evidence,
                                              const std::filesystem::path &rom) {
  ayther::AytherSession::Config config;
  config.provider = ayther::EmulationProvider::tessera_native;
  config.core_path = "legacy-core-must-not-open.dll";
  config.enable_audio = false;
  config.derive_core_pack = false;
  config.native_config.backend_id = "clownmdemu";
  config.native_config.system_override = 1;
  config.native_config.content = read(rom);
  config.native_config.config = read(evidence / "config.bin");
  config.native_config.initial_persistent_memory = read(evidence / "memory.bin");
  native::NativeResult created;
  auto session = ayther::AytherSession::create(config, created);
  require(static_cast<bool>(session),
          "ENGINE_SESSION_CREATE_FAILED:" + std::to_string(created.code));
  ok(created);
  auto owner = std::move(*session);
  require(owner && owner->native_emulation(), "NATIVE_PROVIDER_NOT_SELECTED");
  return owner;
}
void work(const std::filesystem::path &evidence,
          const std::filesystem::path &rom, const std::string &scenario,
          unsigned checkpoint_frame, bool fx02, bool action) {
  auto owner = create(evidence, rom);
  auto &machine = *owner->native_emulation();
  const auto trace = read(evidence / (scenario + ".trace"));
  if (checkpoint_frame) {
    const auto checkpoint = read(evidence / (scenario + "-checkpoints") /
                                 (std::to_string(checkpoint_frame) + ".tsc"));
    ok(machine.restore_checkpoint(checkpoint, trace));
  } else {
    ok(machine.bind_trace(trace));
  }
  native::NativeSessionInfo initial;
  ok(machine.info(initial));
  require(initial.system == 1 && initial.frame_id.frame == checkpoint_frame &&
              initial.next_replay_frame == checkpoint_frame + 1,
          "ENGINE_RESTORE_CURSOR_INVALID");
  const std::array<native::NativeControlItem, 2> controls{{
      {1, 1, 15}, {2, 1, 31}}};
  ok(machine.submit_controls({initial.frame_id.epoch,
                              initial.frame_id.frame + 1,
                              900 + checkpoint_frame, controls}));
  const auto reference = json(evidence / (scenario + "-full.json")).at("frames");
  require(reference.size() == 3000, "REFERENCE_INCOMPLETE");
  const auto count = checkpoint_frame ? 500U : 3000U;
  unsigned oracle_checks = 0;
  for (unsigned index = 0; index < count; ++index) {
    ok(machine.replay_next());
    native::NativeSessionInfo info;
    ok(machine.info(info));
    const auto frame_number = checkpoint_frame + index + 1;
    require(info.frame_id.frame == frame_number &&
                info.next_replay_frame == frame_number + 1,
            "ENGINE_REPLAY_FRAME_INVALID");
    Bytes state;
    ok(machine.state(state));
    native::NativeFrame original;
    ok(machine.original(original));
    const auto &borrowed = owner->native_frame();
    require(borrowed.native_result.code == 0 &&
                borrowed.fb_pixels == original.pixels.data() &&
                borrowed.native_original.pcm.data() == original.pcm.data(),
            "ENGINE_ORIGINAL_NOT_BORROWED");
    const auto &expected = reference[frame_number - 1];
    require(expected.at("frame") == frame_number &&
                expected.at("state") == digest(state) &&
                expected.at("video") == frame_digest("TSVIDEO1", original, true) &&
                expected.at("audio") == frame_digest("TSAUDIO1", original, false),
            "ENGINE_TESSERA_CANONICAL_DIVERGENCE:frame=" +
                std::to_string(frame_number));
    if (fx02 && ((frame_number >= 100 && frame_number <= 200) ||
                 (frame_number >= 350 && frame_number <= 550) ||
                 (frame_number >= 650 && frame_number <= 850))) {
      oracle(machine, original, action && frame_number >= 350 &&
                                     frame_number <= 550);
      ++oracle_checks;
    }
  }
  if (checkpoint_frame == 0 || checkpoint_frame == 2500)
    require(machine.replay_next().code == 1, "ENGINE_EOF_MISSING");
  require(oracle_checks == (fx02 ? checkpoint_frame == 0 ? 503U
                                   : checkpoint_frame == 500 ? 251U : 0U
                                : 0U),
          "ENGINE_FX02_ORACLE_COVERAGE_WRONG");
  std::cout << "ENGINE_CHECKPOINT_PASS scenario=" << scenario
            << " from=" << checkpoint_frame << " frames=" << count
            << " oracle=" << oracle_checks << '\n';
}
std::wstring quote(std::wstring_view text) {
  std::wstring result = L"\"";
  for (const auto ch : text) {
    if (ch == L'"') result += L'\\';
    result += ch;
  }
  return result + L'"';
}
void spawn(const std::filesystem::path &self,
           const std::vector<std::wstring> &arguments) {
  std::wstring command = quote(self.wstring());
  for (const auto &argument : arguments) command += L" " + quote(argument);
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  require(CreateProcessW(self.c_str(), command.data(), nullptr, nullptr, FALSE,
                         CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                         &process) != FALSE, "ENGINE_WORKER_CREATE_FAILED");
  const auto wait = WaitForSingleObject(process.hProcess, 600000);
  if (wait != WAIT_OBJECT_0) TerminateProcess(process.hProcess, 124);
  DWORD status = 0;
  GetExitCodeProcess(process.hProcess, &status);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  require(wait == WAIT_OBJECT_0 && status == 0,
          "ENGINE_WORKER_FAILED:" + std::to_string(status));
}
} // namespace

int main(int argc, char **argv) {
  try {
    require(argc == 3 || argc == 7, "ARGUMENTS_INVALID");
    const std::filesystem::path evidence = argv[1];
    const std::filesystem::path original = argv[2];
    if (argc == 7) {
      work(evidence, argv[3], argv[4], std::stoul(argv[5]),
           std::string_view(argv[4]) != "original",
           std::string_view(argv[6]) == "action");
      return 0;
    }
    require(json(evidence / "summary.json").at("divergences") == 0,
            "TESSERA_BASELINE_NOT_APPROVED");
    const auto fx02 = evidence / "fx02" / "program.md";
    for (const auto &scenario : {std::string("original"),
                                 std::string("fx02-control"),
                                 std::string("fx02-action")}) {
      const auto rom = scenario == "original" ? original : fx02;
      for (unsigned frame : {0U, 500U, 1500U, 2500U}) {
        spawn(argv[0], {evidence.wstring(), original.wstring(), rom.wstring(),
                        std::wstring(scenario.begin(), scenario.end()),
                        std::to_wstring(frame),
                        scenario == "fx02-action" ? L"action" : L"control"});
      }
    }
    std::cout << "ENGINE_PHASE3_PASS processes=12 frames=13500\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "ENGINE_PHASE3_FAILED: " << error.what() << '\n';
    return 1;
  }
}
