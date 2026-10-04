// Spec 002, BR-030 (RF-3.6): the visual state contract (contracts.md C4)
// compiles alone and validates headers in the contract's order.
#include <ayther/engine/visual_state.hpp>

#include <cstdint>

namespace visual = ayther::engine::visual_state;
using Code = visual::VisualStateRestoreCode;
using Section = visual::VisualStateSection;

static_assert(visual::kVisualStateVersion == visual::VisualStateVersion{1, 0});
static_assert(visual::kVisualStateRequiredSections ==
              (visual::visual_state_section(Section::sprite_tweens) |
               visual::visual_state_section(Section::screen_recognition) |
               visual::visual_state_section(Section::level_camera) |
               visual::visual_state_section(Section::palette_luma) |
               visual::visual_state_section(Section::previous_audio_mask) |
               visual::visual_state_section(Section::palette_signature) |
               visual::visual_state_section(Section::animation_grouper) |
               visual::visual_state_section(Section::plane_sequence_clocks) |
               visual::visual_state_section(Section::cinematic) |
               visual::visual_state_section(Section::hd_animation_phase) |
               visual::visual_state_section(Section::panorama_tint)));

namespace {
constexpr visual::VisualStateHeader header_with(std::uint16_t major,
                                                std::uint32_t sections) {
  visual::VisualStateHeader h;
  h.version = {major, 0};
  h.emulation_frame = 7;
  h.sections = sections;
  return h;
}
} // namespace

static_assert(visual::validate_visual_state_header(
                  header_with(2, visual::kVisualStateRequiredSections), "x", 7)
                  .code == Code::unsupported_version);
static_assert(visual::validate_visual_state_header(
                  header_with(1, visual::kVisualStateRequiredSections), "x", 7)
                  .code == Code::identity_mismatch);
static_assert(visual::validate_visual_state_header(
                  header_with(1, visual::kVisualStateRequiredSections), "", 7)
                  .code == Code::identity_mismatch);

namespace {
// Runtime half of the probe (std::string comparisons): the integration tests
// exercise the same function through the session.
[[maybe_unused]] int visual_state_header_probe() {
  visual::VisualStateHeader h =
      header_with(1, visual::kVisualStateRequiredSections);
  h.game_state_identity = "abc";
  int failures = 0;
  failures +=
      visual::validate_visual_state_header(h, "abc", 7).restored() ? 0 : 1;
  failures += visual::validate_visual_state_header(h, "abc", 8).code ==
                      Code::frame_mismatch
                  ? 0
                  : 1;
  h.sections = 1;
  failures += visual::validate_visual_state_header(h, "abc", 7).code ==
                      Code::missing_sections
                  ? 0
                  : 1;
  h.sections = visual::kVisualStateRequiredSections | (1U << 20);
  failures +=
      visual::validate_visual_state_header(h, "abc", 7).unknown_sections ==
              (1U << 20)
          ? 0
          : 1;
  return failures;
}
} // namespace
