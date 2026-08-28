#pragma once
// ---------------------------------------------------------------------------
// ayther_sdk_version.h — SDK version and compatibility contract.
//
// AYTHER has independent core-ABI, emulator-extension, and pack-format version
// axes. This header defines the native SDK version that installed consumers
// compile against.
//
// SemVer rule for the current 0.x series: a minor-version change may break the
// contract. The CMake package enforces the same rule with SameMinorVersion.
//
// Two values must remain distinguishable:
//
//   - compile-time: `AYTHER_SDK_VERSION_*` from the installed headers;
//   - link/run-time: `ayther::sdk_version()` from the linked library.
//
// They can differ when a program finds an unexpected library at run time.
// `sdk_version_check()` reports both values so incompatibility fails with a
// diagnostic rather than at the first changed layout or call contract.
// ---------------------------------------------------------------------------
#include <cstdint>
#include <string>

// Canonical native SDK version. CMake and Cargo metadata must match it; the
// version test rejects drift between these declarations.
#define AYTHER_SDK_VERSION_MAJOR 0
#define AYTHER_SDK_VERSION_MINOR 1
#define AYTHER_SDK_VERSION_PATCH 0

#define AYTHER_SDK_STRINGIFY_(x) #x
#define AYTHER_SDK_STRINGIFY(x)  AYTHER_SDK_STRINGIFY_(x)
#define AYTHER_SDK_VERSION_STRING            \
    AYTHER_SDK_STRINGIFY(AYTHER_SDK_VERSION_MAJOR) "." \
    AYTHER_SDK_STRINGIFY(AYTHER_SDK_VERSION_MINOR) "." \
    AYTHER_SDK_STRINGIFY(AYTHER_SDK_VERSION_PATCH)

/// Comparable integer: MAJOR*10000 + MINOR*100 + PATCH.
#define AYTHER_SDK_VERSION_NUMBER                    \
    (AYTHER_SDK_VERSION_MAJOR * 10000 +              \
     AYTHER_SDK_VERSION_MINOR * 100   +              \
     AYTHER_SDK_VERSION_PATCH)

namespace ayther {

struct SdkVersion {
    uint32_t major = 0, minor = 0, patch = 0;
    constexpr uint32_t number() const noexcept {
        return major * 10000 + minor * 100 + patch;
    }
    std::string str() const {
        return std::to_string(major) + "." + std::to_string(minor) + "." +
               std::to_string(patch);
    }
};

/// Returns the version of the linked library, not the including header.
/// The out-of-line definition is required for mismatch detection.
[[nodiscard]] SdkVersion sdk_version() noexcept;

/// Returns the header version used to compile the current translation unit.
[[nodiscard]] constexpr SdkVersion sdk_headers_version() noexcept {
    return SdkVersion{ AYTHER_SDK_VERSION_MAJOR,
                       AYTHER_SDK_VERSION_MINOR,
                       AYTHER_SDK_VERSION_PATCH };
}

/// Compares compile-time and linked-library SDK versions.
///
/// During 0.x development, major and minor must match; patch versions may
/// differ. Returns an empty string on compatibility, otherwise a diagnostic.
[[nodiscard]] inline std::string sdk_version_check(
        const SdkVersion& compiled, const SdkVersion& linked) {
    if (compiled.major == linked.major && compiled.minor == linked.minor) {
        return {};
    }
    const char* rule = compiled.major == 0
        ? " (during 0.x, a minor-version change can break compatibility)"
        : " (different major versions indicate a changed contract)";
    return "AYTHER SDK incompatible: compiled against " + compiled.str() +
           ", linked against " + linked.str() + rule;
}

/// Compares this translation unit's headers with the linked library.
[[nodiscard]] inline std::string sdk_version_check() {
    return sdk_version_check(sdk_headers_version(), sdk_version());
}

}  // namespace ayther
