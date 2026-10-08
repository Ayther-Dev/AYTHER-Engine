// Regression for the internal parsed-sprite consumers. The public diagnostic
// accessor intentionally retains its uint8_t count, but scene inventory and
// pose depth must consume the observer's exact uint32_t ABI view.
#include <ayther/ayther_session.h>

#include "pose_anchor_scene.h"
#include "pose_pack_fixture.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr char kDenseScenario[] = "AYTHER-256-PARSED-SPRITES";
constexpr char kSparseScenario[] = "AYTHER-SPARSE-PARSED-SPRITES";
constexpr std::uint32_t kProducerCount = 256;

int failures = 0;

void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}

constexpr std::uint8_t expected_chain(std::uint32_t producer) {
  return static_cast<std::uint8_t>((producer * 29U + 7U) % 200U);
}

std::filesystem::path scenario_rom(const char *stem, const char *tag) {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / (std::string(stem) + ".md");
  std::vector<char> bytes(0x10000, 0);
  std::copy(tag, tag + std::char_traits<char>::length(tag), bytes.begin());
  std::ofstream out(path, std::ios::binary);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  check(static_cast<bool>(out), "the parsed-sprite scenario ROM is written");
  return path;
}

const ayther::SceneElement *
find_element(const std::vector<ayther::SceneElement> &inventory,
             const AytherSpriteOccurrence &occurrence) {
  const auto found = std::find_if(
      inventory.begin(), inventory.end(), [&](const auto &element) {
        return element.layer == 3 && element.hash == occurrence.hash &&
               element.slot == occurrence.slot &&
               element.x == occurrence.screen_x &&
               element.y == occurrence.screen_y;
      });
  return found == inventory.end() ? nullptr : &*found;
}

void check_legacy_accessor(ayther::AytherSession &session) {
  std::uint8_t legacy_count = 0;
  const std::uint8_t *legacy = session.parsed_sprites_raw(&legacy_count);
  check(legacy != nullptr && legacy_count == 255,
        "the public legacy accessor remains saturated at 255 entries");
}

void dense_256_producers_keep_last_identity() {
  const auto rom = scenario_rom("ayther_256_parsed_sprites", kDenseScenario);
  auto session = ayther::test::open_pose_session(rom.string());
  check(session != nullptr, "the 256-producer ABI scenario opens");
  if (!session)
    return;

  const ayther::FrameView &frame = session->step();
  check(frame.sprite_occ_count == kProducerCount,
        "all 256 ABI producers become sprite occurrences");
  if (frame.sprite_occ_count != kProducerCount)
    return;

  const AytherSpriteOccurrence penultimate = frame.sprite_occs[254];
  const AytherSpriteOccurrence last = frame.sprite_occs[255];
  check(last.slot == 15 && last.link == expected_chain(255),
        "producer 255 reaches the occurrence boundary with its ABI identity");
  check_legacy_accessor(*session);

  std::vector<ayther::SceneElement> inventory;
  session->scene_inventory(inventory);
  const ayther::SceneElement *last_element = find_element(inventory, last);
  check(last_element != nullptr && last_element->pattern == 256 &&
            last_element->chain == expected_chain(255),
        "scene_inventory consumes occurrence 255 from the exact uint32 view");

  const int pose_width =
      last.screen_x + last.w_tiles * 8 - penultimate.screen_x;
  const int pose_top = (std::min)(penultimate.screen_y, last.screen_y);
  const int pose_height =
      (std::max)(penultimate.screen_y + penultimate.h_tiles * 8,
                 last.screen_y + last.h_tiles * 8) -
      pose_top;
  ayther::test::PosePackFixture pack("parsed_sprites_256_pose");
  pack.add_asset("graphics/pose.png",
                 ayther::test::solid_png(pose_width, pose_height, 0x00FF00FFU));
  const std::string poses =
      "[[pose]]\nhashes = [\"" + ayther::test::hash_hex(penultimate.hash) +
      "\", \"" + ayther::test::hash_hex(last.hash) +
      "\"]\nasset = \"graphics/pose.png\"\nrel = \"0,0|" +
      std::to_string(last.screen_x - penultimate.screen_x) + "," +
      std::to_string(last.screen_y - penultimate.screen_y) + "\"\ndims = \"" +
      std::to_string(penultimate.w_tiles * 8) + "," +
      std::to_string(penultimate.h_tiles * 8) + "|" +
      std::to_string(last.w_tiles * 8) + "," +
      std::to_string(last.h_tiles * 8) + "\"\n";
  std::string error;
  check(pack.bake(poses, error), "the boundary two-member pose pack bakes");
  session.reset();
  auto packed = ayther::test::open_pose_session(rom.string(), pack.pack_path(),
                                                pack.registry_path());
  check(packed != nullptr && packed->pack().is_valid(),
        "the 256-producer scenario opens with the pose pack");
  if (!packed || !packed->pack().is_valid()) {
    std::printf("  %s\n", error.c_str());
    return;
  }

  const ayther::FrameView &packed_frame = packed->step();
  check(packed_frame.sprite_sub_count >= 1,
        "the pose containing occurrence 255 is applied");
  bool last_partition_has_exact_chain = false;
  for (std::uint32_t index = 0; packed_frame.sprite_partitions &&
                                index < packed_frame.sprite_partition_count;
       ++index) {
    const ayther::SpritePartition &partition =
        packed_frame.sprite_partitions[index];
    const bool covers_last =
        last.screen_x >= partition.x &&
        last.screen_x < partition.x + static_cast<int>(partition.w) &&
        last.screen_y >= partition.y &&
        last.screen_y < partition.y + static_cast<int>(partition.h);
    last_partition_has_exact_chain = last_partition_has_exact_chain ||
                                     (partition.sub == 0 && covers_last &&
                                      partition.chain == expected_chain(255));
  }
  check(last_partition_has_exact_chain,
        "pose partitions consume occurrence 255 from the exact uint32 view");
}

void sparse_records_before_occurrence_255_keep_alignment() {
  const auto rom =
      scenario_rom("ayther_sparse_parsed_sprites", kSparseScenario);
  auto session = ayther::test::open_pose_session(rom.string());
  check(session != nullptr, "the sparse 260-record ABI scenario opens");
  if (!session)
    return;

  const ayther::FrameView &frame = session->step();
  check(frame.sprite_occ_count == kProducerCount,
        "260 source records with non-producers and duplicates yield 256 "
        "occurrences");
  if (frame.sprite_occ_count != kProducerCount)
    return;
  check_legacy_accessor(*session);

  const AytherSpriteOccurrence last = frame.sprite_occs[255];
  std::vector<ayther::SceneElement> inventory;
  session->scene_inventory(inventory);
  const ayther::SceneElement *last_element = find_element(inventory, last);
  check(last_element != nullptr && last_element->pattern == 256 &&
            last_element->chain == expected_chain(255),
        "records beyond raw index 255 remain aligned with occurrence 255");
}

} // namespace

int main() try {
  dense_256_producers_keep_last_identity();
  sparse_records_before_occurrence_255_keep_alignment();
  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
