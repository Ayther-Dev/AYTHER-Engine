#pragma once
#include <string>
#include <cstdio>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
   using NativeLib = HMODULE;
#else
#  include <dlfcn.h>
   using NativeLib = void*;
#endif

/// @brief Move-disabled RAII owner for one dynamically loaded libretro core.
///
/// Resolved symbols are borrowed function pointers and become invalid after
/// unload() or destruction. Callers must validate every required symbol before
/// invoking the core. Loading a library executes platform loader behavior and
/// therefore crosses a trust boundary.
// Platform-agnostic dynamic loader for a Libretro core (.dll / .so / .dylib).
// Usage:
//   CoreLoader loader;
//   loader.load("genesis_plus_gx_libretro.dll");
//   auto fn = loader.sym<void(*)()>("retro_init");
class CoreLoader {
public:
    CoreLoader()  = default;
    ~CoreLoader() { unload(); }

    CoreLoader(const CoreLoader&)            = delete;
    CoreLoader& operator=(const CoreLoader&) = delete;

    bool load(const std::string& path) {
#ifdef _WIN32
        lib_ = LoadLibraryA(path.c_str());
        if (!lib_) {
            std::fprintf(stderr, "[CoreLoader] LoadLibraryA('%s') failed: %lu\n",
                         path.c_str(), GetLastError());
            return false;
        }
#else
        lib_ = dlopen(path.c_str(), RTLD_LAZY);
        if (!lib_) {
            std::fprintf(stderr, "[CoreLoader] dlopen('%s') failed: %s\n",
                         path.c_str(), dlerror());
            return false;
        }
#endif
        std::fprintf(stdout, "[CoreLoader] Loaded: %s\n", path.c_str());
        return true;
    }

    void unload() {
        if (!lib_) return;
#ifdef _WIN32
        FreeLibrary(lib_);
#else
        dlclose(lib_);
#endif
        lib_ = nullptr;
    }

    bool is_loaded() const { return lib_ != nullptr; }

    // Retrieve a typed function pointer by symbol name.
    template<typename FnPtr>
    FnPtr sym(const char* name) const {
#ifdef _WIN32
        auto ptr = reinterpret_cast<FnPtr>(
            reinterpret_cast<void*>(GetProcAddress(lib_, name)));
#else
        auto ptr = reinterpret_cast<FnPtr>(dlsym(lib_, name));
#endif
        if (!ptr)
            std::fprintf(stderr, "[CoreLoader] Symbol not found: %s\n", name);
        return ptr;
    }

private:
    NativeLib lib_ = nullptr;
};
