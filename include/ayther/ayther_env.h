#pragma once
// ---------------------------------------------------------------------------
// ayther_env.h — getenv with a fallback to the legacy AETHER_ prefix (code
// rebrand 2026-07-25): older scripts and harnesses that export AETHER_* keep
// working unchanged. ALWAYS use this for AYTHER_* variables.
// ---------------------------------------------------------------------------
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace ayther {

namespace detail {

/// Windows marks `std::getenv` deprecated and offers `getenv_s`, which COPIA a
/// memoria del llamador en vez de devolver un puntero al bloque de entorno. Esa
/// copia necesita dueño, y el dueño no puede ser un buffer único compartido:
/// hay llamadores que sostienen el resultado mientras leen OTRA variable, y con
/// un solo buffer el primer puntero les queda colgando. Cada nombre se lleva su
/// propia ranura —los nodos de std::map son estables— así que el puntero vive
/// tanto como el hilo y sólo lo mueve releer ESA misma variable.
inline const char* env_get_exact(const char* name) {
#ifdef _WIN32
    size_t required = 0;
    if (::getenv_s(&required, nullptr, 0, name) != 0 || required == 0) {
        return nullptr;
    }
    thread_local std::map<std::string, std::string> values;
    std::string& slot = values[name];
    slot.resize(required);
    if (::getenv_s(&required, slot.data(), slot.size(), name) != 0 ||
        required == 0) {
        return nullptr;
    }
    slot.resize(required - 1);   // getenv_s cuenta el NUL; std::string no.
    return slot.c_str();
#else
    return std::getenv(name);
#endif
}

}  // namespace detail

inline const char* env_get(const char* name) {
    if (const char* v = detail::env_get_exact(name)) return v;
    if (std::strncmp(name, "AYTHER_", 7) == 0) {
        const std::string legacy = std::string("AETHER_") + (name + 7);
        return detail::env_get_exact(legacy.c_str());
    }
    return nullptr;
}

}  // namespace ayther
