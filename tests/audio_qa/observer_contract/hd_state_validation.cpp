#include <ayther/engine/audio_hd_state.hpp>

#include <string>

namespace obs = ayther::engine::audio_observation;

namespace {

bool rejected(const obs::AudioHdStateHeader &state,
              obs::AudioHdStateValidationCode expected,
              std::uint32_t missing = 0, std::uint32_t unknown = 0) noexcept {
  const auto result = obs::validate_audio_hd_state(state, "state-17", 42);
  return !result.compatible() && result.code == expected &&
         result.missing_sections == missing &&
         result.unknown_sections == unknown;
}

} // namespace

int main() { // NOLINT(bugprone-exception-escape) -- Test allocation failure is
             // fatal.
  obs::AudioHdStateHeader complete;
  complete.game_state_identity = "state-17";
  complete.emulation_frame = 42;
  complete.sections = obs::kAudioHdStateRequiredSections;
  if (!obs::validate_audio_hd_state(complete, "state-17", 42).compatible())
    return 1;
  if (obs::validate_audio_hd_state(complete, {}, 42).code !=
      obs::AudioHdStateValidationCode::missing_expected_identity)
    return 9;

  auto invalid = complete;
  invalid.version.minor = 1;
  if (!rejected(invalid, obs::AudioHdStateValidationCode::unsupported_version))
    return 2;

  invalid = complete;
  invalid.game_state_identity.clear();
  if (!rejected(invalid, obs::AudioHdStateValidationCode::missing_identity))
    return 3;

  invalid = complete;
  invalid.game_state_identity = "state-18";
  if (!rejected(invalid, obs::AudioHdStateValidationCode::identity_mismatch))
    return 4;

  invalid = complete;
  invalid.emulation_frame = 41;
  if (!rejected(invalid, obs::AudioHdStateValidationCode::frame_mismatch))
    return 5;

  invalid = complete;
  const auto voices =
      obs::audio_hd_state_section(obs::AudioHdStateSection::voices);
  invalid.sections &= ~voices;
  if (!rejected(invalid, obs::AudioHdStateValidationCode::missing_sections,
                voices))
    return 6;

  invalid = complete;
  constexpr std::uint32_t unknown = 1U << 31;
  invalid.sections |= unknown;
  if (!rejected(invalid, obs::AudioHdStateValidationCode::unknown_sections, 0,
                unknown))
    return 7;

  invalid = complete;
  invalid.game_state_identity =
      std::string(obs::kAudioHdStateIdentityLimit + 1, 'x');
  return rejected(invalid, obs::AudioHdStateValidationCode::identity_too_long)
             ? 0
             : 8;
}
