#pragma once
// ---------------------------------------------------------------------------
// ayther_unique_handle.h — RAII for opaque Rust handles.
//
// No raw opaque pointer from ayther_core lives loose in C++ (see
// docs/API_COMPATIBILITY.md#ownership-and-lifetime). Every handle is wrapped at
// creation so Rust frees it deterministically on scope exit — even through an
// early return — which matters during hot-reload / scene restart.
//
// The deleter is the matching `_free()` function, carried as a *stateless*
// template parameter, so the wrapper is exactly pointer-sized (zero overhead).
//
// Usage:
//   using TileHasherPtr =
//       ayther::unique_handle<AytherTileHasher, &ayther_tile_hasher_free>;
//   TileHasherPtr h{ ayther_tile_hasher_new() };   // freed automatically
// ---------------------------------------------------------------------------

#include <memory>

namespace ayther {

/// Stateless deleter that invokes the free-function `Free` (C++17 `auto` NTTP).
template <auto Free>
struct handle_deleter {
    template <class T>
    void operator()(T* p) const noexcept {
        if (p) Free(p);
    }
};

/// unique_ptr specialized to call the runtime's `_free()` for `T`.
template <class T, auto Free>
using unique_handle = std::unique_ptr<T, handle_deleter<Free>>;

}  // namespace ayther
