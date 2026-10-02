#include <ayther/ayther_components_toml.h>
#include <ayther/engine/engine.hpp>

int main() {
  const auto version = ayther::engine::version();
  const auto continuity = ayther::parse_audio_continuity_toml(
      "[[event]]\nsignature=\"0x1\"\nasset=\"a.wav\"\n"
      "continuity_schema=1\ncategory=\"music\"\n"
      "repeat_policy=\"continue\"\ntransition_policy=\"cut\"\n"
      "max_voices=1\nexclusive_bus=true\nbus=\"music\"\n");
  return version.major == 0U && version.minor == 1U && version.patch == 0U &&
                 continuity.ok && continuity.values.size() == 1
             ? 0
             : 1;
}
