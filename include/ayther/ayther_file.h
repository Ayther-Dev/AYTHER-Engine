#pragma once
// ---------------------------------------------------------------------------
// ayther_file.h — fopen sin la deprecación de la CRT de Windows.
//
// NO se usa `fopen_s`: abre SIN COMPARTIR, y eso no es lo mismo que `fopen`.
// Con el tee de audio puesto, el WAV lo escribe una sesión mientras el oráculo
// lo mide, y con la apertura exclusiva la segunda mano se queda sin archivo —
// que fue exactamente cómo se cayó audio_output al migrar. `_fsopen` con
// `_SH_DENYNO` es la que conserva el reparto de `fopen`, y no está deprecada.
// ---------------------------------------------------------------------------
#include <cstdio>

#ifdef _WIN32
#include <share.h>
#endif

namespace ayther {

[[nodiscard]] inline std::FILE* file_open(
    const char* path, const char* mode) noexcept {
#ifdef _WIN32
    return ::_fsopen(path, mode, _SH_DENYNO);
#else
    return std::fopen(path, mode);
#endif
}

}  // namespace ayther
