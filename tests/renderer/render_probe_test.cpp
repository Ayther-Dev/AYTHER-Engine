// Spec 002, BR-035 (RF-3.5): the render probe reproduces a take without a
// window and writes, per frame, a JSON with the frame index and whether the
// frame could be composed. The take is recorded here with the test core.
#include <ayther/ayther_recording.h>
#include <ayther/ayther_session.h>

#include "../../tools/common/synth_rom.h"
#include "mini_json.h"
#include "render_frame_contract.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}

namespace fs = std::filesystem;

std::string read_text(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool contains(const std::string &text, const std::string &needle) {
  return text.find(needle) != std::string::npos;
}

// Records `frames` frames of the test core with a deterministic input.
bool record_take(const std::string &rom, const fs::path &out, int frames) {
  ayther::AytherSession::Config config;
  config.core_path = AYTHER_TEST_CORE_PATH;
  config.rom_path = rom;
  config.enable_audio = false;
  config.derive_core_pack = false;
  auto created = ayther::AytherSession::create(config);
  if (!created)
    return false;
  std::unique_ptr<ayther::AytherSession> s = std::move(*created);
  s->record_start();
  for (int f = 0; f < frames; ++f) {
    s->set_input(0, static_cast<std::uint16_t>((f * 7) & 0xFF));
    (void)s->step();
  }
  s->record_stop();
  return s->take_recording().save(out.string());
}

int run_probe(const std::string &args) {
#ifdef _WIN32
  const std::string command =
      std::string("\"\"") + RENDER_PROBE_PATH + "\" " + args + "\"";
#else
  const std::string command =
      std::string("\"") + RENDER_PROBE_PATH + "\" " + args;
#endif
  // The probe is a separate executable by design (it is what QA runs); the
  // test is single-threaded and builds the command from its own paths.
  // NOLINTNEXTLINE(bugprone-command-processor,concurrency-mt-unsafe)
  return std::system(command.c_str());
}
} // namespace

int main() try {
  const fs::path dir = fs::temp_directory_path() / "ayther_render_probe_test";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  const std::string rom = ayther::synth::canonical_rom_path();
  const fs::path take = dir / "take.arp";
  check(!rom.empty() && record_take(rom, take, 40),
        "a 40-frame take of the test core is recorded");

  const fs::path out = dir / "out";
  const std::string args =
      "--rom \"" + rom + "\" --core \"" + AYTHER_TEST_CORE_PATH +
      "\" --take \"" + take.string() + "\" --no-pack --frames 10-19 --out \"" +
      out.string() + "\"";
  check(run_probe(args) == 0, "render_probe reproduces frames 10-19");
  bool all = true;
  bool composability = true;
  for (int f = 10; f <= 19; ++f) {
    char name[32];
    std::snprintf(name, sizeof(name), "frame_%06d.json", f);
    const std::string json = read_text(out / name);
    all = all && contains(json, "\"frame\": " + std::to_string(f));
    composability = composability && contains(json, "\"composability\": \"");
  }
  check(all, "RF-3.5: one JSON per frame names its frame index");
  // BR-037 (RF-7.2, RF-7.3): every record holds the observation and conforms
  // to the render observation contract.
  bool conform = true;
  bool observed = true;
  for (int f = 10; f <= 19; ++f) {
    char name[32];
    std::snprintf(name, sizeof(name), "frame_%06d.json", f);
    const auto record = ayther::test::json::parse(read_text(out / name));
    const std::vector<std::string> errors =
        record ? ayther::test::validate_frame_record(*record)
               : std::vector<std::string>{"not JSON"};
    for (const std::string &e : errors)
      std::printf("  frame %d: %s\n", f, e.c_str());
    conform = conform && errors.empty();
    observed = observed && record && record->get("occurrences") != nullptr &&
               !record->get("occurrences")->items.empty();
  }
  check(conform, "RF-7.2: each frame record conforms to the contract");
  check(observed, "RF-7.2: the records carry the frame's occurrences");
  check(composability, "RF-3.5: each JSON says whether the frame composed");
  check(!fs::exists(out / "frame_000009.json") &&
            !fs::exists(out / "frame_000020.json"),
        "only the requested frames are written");

  check(run_probe("--rom \"" + rom + "\" --no-pack --out \"" + out.string() +
                  "\"") != 0,
        "missing arguments are rejected");

  // BR-036 (RF-3.5): composed image and core framebuffer as PNG, identical
  // across two runs without pack.
  const fs::path again = dir / "again";
  const std::string args_again =
      "--rom \"" + rom + "\" --core \"" + AYTHER_TEST_CORE_PATH +
      "\" --take \"" + take.string() + "\" --no-pack --frames 10-19 --out \"" +
      again.string() + "\"";
  check(run_probe(args_again) == 0, "a second run of the same frames");
  bool pngs = true;
  bool identical = true;
  for (int f = 10; f <= 19; ++f) {
    for (const char *kind : {"composed", "core"}) {
      char name[40];
      std::snprintf(name, sizeof(name), "%s_%06d.png", kind, f);
      const std::string first = read_text(out / name);
      pngs = pngs && first.size() > 8 && first.compare(1, 3, "PNG") == 0;
      identical = identical && first == read_text(again / name);
    }
  }
  check(pngs, "RF-3.5: each frame has a composed PNG and a core PNG");
  check(identical, "RF-3.5: two runs without pack give identical PNGs");

  // BR-038 (RF-3.5): --check o1 reports, per frame, whether the composed
  // image is the core's framebuffer pixel for pixel.
  const fs::path o1 = dir / "o1";
  check(run_probe("--rom \"" + rom + "\" --core \"" + AYTHER_TEST_CORE_PATH +
                  "\" --take \"" + take.string() +
                  "\" --no-pack --frames 10-12 --check o1 --out \"" +
                  o1.string() + "\"") == 0,
        "render_probe --check o1 runs");
  bool reported = true;
  for (int f = 10; f <= 12; ++f) {
    char name[32];
    std::snprintf(name, sizeof(name), "frame_%06d.json", f);
    const auto record = ayther::test::json::parse(read_text(o1 / name));
    const ayther::test::json::Value *result =
        record ? record->get("o1") : nullptr;
    reported = reported && result != nullptr &&
               result->get("identical") != nullptr &&
               result->get("differing_pixels") != nullptr &&
               (result->get("identical")->boolean ==
                (result->get("differing_pixels")->number == 0.0));
  }
  check(reported, "RF-3.5: each frame reports O1 with its differing pixels");

  // BR-039: --check o2 evaluates the six invariants per frame. Without pack
  // every sprite is drawn as its original: no violations.
  const fs::path o2 = dir / "o2";
  check(run_probe("--rom \"" + rom + "\" --core \"" + AYTHER_TEST_CORE_PATH +
                  "\" --take \"" + take.string() +
                  "\" --no-pack --frames 10-14 --check o2 --out \"" +
                  o2.string() + "\"") == 0,
        "render_probe --check o2 runs");
  bool audited = true;
  for (int f = 10; f <= 14; ++f) {
    char name[32];
    std::snprintf(name, sizeof(name), "frame_%06d.json", f);
    const auto record = ayther::test::json::parse(read_text(o2 / name));
    const ayther::test::json::Value *result =
        record ? record->get("o2") : nullptr;
    const ayther::test::json::Value *violations =
        result ? result->get("violations") : nullptr;
    audited = audited && violations != nullptr && violations->items.empty();
  }
  check(audited, "RF-10.3: without pack O2 finds no violation in any frame");

  fs::remove_all(dir, ec);
  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
