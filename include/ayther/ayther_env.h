#pragma once
// ---------------------------------------------------------------------------
// ayther_env.h — getenv with a fallback to the legacy AETHER_ prefix (code
// rebrand 2026-07-25): older scripts and harnesses that export AETHER_* keep
// working unchanged. ALWAYS use this for AYTHER_* variables.
// ---------------------------------------------------------------------------
#include <cstdlib>
#include <cstring>
#include <string>

namespace ayther {

inline const char* env_get(const char* name) {
    if (const char* v = std::getenv(name)) return v;
    if (std::strncmp(name, "AYTHER_", 7) == 0) {
        const std::string legacy = std::string("AETHER_") + (name + 7);
        return std::getenv(legacy.c_str());
    }
    return nullptr;
}

}  // namespace ayther
