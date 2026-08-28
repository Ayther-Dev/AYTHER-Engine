#pragma once
// ---------------------------------------------------------------------------
// ayther_env.h — getenv con fallback al prefijo legacy AETHER_ (rebrand de
// código 2026-07-25): scripts/arneses viejos que exporten AETHER_* siguen
// funcionando sin cambios. Usar SIEMPRE esto para variables AYTHER_*.
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
