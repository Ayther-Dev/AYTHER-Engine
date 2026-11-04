// Spec 008 (TP-008.6, D-4): the emulation generation published by a session —
// emulator id, version and determinism contract — for both providers.
#include <ayther/emulation_generation.h>

#include <array>
#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    std::printf("  [%s] %s\n", condition ? " OK " : "FAIL", message);
    if (!condition) {
        ++failures;
    }
}

}  // namespace

int main() {
    using namespace ayther;

    // libretro provider: Genesis Plus GX is `gpgx`, with the core's own version.
    const auto gpgx = libretro_generation("Genesis Plus GX", "1.7.5-ayther-r4");
    check(gpgx.id == "gpgx", "Genesis Plus GX -> id gpgx");
    check(gpgx.version == "1.7.5-ayther-r4", "gpgx keeps the core version");
    check(gpgx.determinism_contract == kGpgxDeterminismContract &&
              kGpgxDeterminismContract == 1U,
          "gpgx determinism contract is 1");

    // Any other libretro core is slugged to printable ASCII, <= 32 bytes.
    const auto other = libretro_generation("Snes9x 2010!", "v1");
    check(other.id == "snes9x2010", "unknown core -> lowercase alphanumerics");
    const auto long_name = libretro_generation(std::string(80, 'A'), "v");
    check(long_name.id.size() == 32 && long_name.id == std::string(32, 'a'),
          "a long library name is cut to 32 bytes");
    const auto nameless = libretro_generation("", "");
    check(nameless.id == "libretro", "an empty or symbol-only name -> libretro");
    const auto accented = libretro_generation("Genesis Plus GX", std::string(80, 'v'));
    check(accented.version.size() == 64, "the version is cut to 64 bytes");
    const auto control = libretro_generation("x", "a\nb\tc");
    check(control.version == "abc", "non-printable version bytes are dropped");

    // Tessera provider: id `tessera`, version = hex of the backend build id.
    std::array<std::uint8_t, 32> build_id{};
    for (std::size_t i = 0; i < build_id.size(); ++i) {
        build_id[i] = static_cast<std::uint8_t>(i * 7U);
    }
    const auto tessera = tessera_generation(build_id);
    check(tessera.id == "tessera", "native provider -> id tessera");
    check(tessera.version.size() == 64 &&
              tessera.version.substr(0, 6) == "00070e",
          "version is the 64-hex build id");
    check(tessera.determinism_contract == kTesseraDeterminismContract &&
              kTesseraDeterminismContract == 1U,
          "tessera determinism contract is 1");

    // The two generations are distinguishable (they gate cross-provider reuse).
    check(!(gpgx == tessera), "gpgx and tessera generations differ");
    check(gpgx == libretro_generation("Genesis Plus GX", "1.7.5-ayther-r4"),
          "equal inputs give equal generations");

    if (failures != 0) {
        std::printf("emulation_generation_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("emulation_generation_test: OK\n");
    return 0;
}
