// Consumes the installed core-only package from outside the producer tree.
// Linking is the real assertion — it proves the shipped archive resolves every
// symbol on its own — so this calls into the core rather than only compiling
// against its headers.
#include <ayther/ayther_core_ffi.h>
#include <ayther/ayther_version.h>

#include <cstring>
#include <iostream>

int main() {
    if (ayther_core_version() != AYTHER_CORE_C_ABI_REVISION) {
        std::cerr << "core C ABI revision disagrees with the installed header: "
                  << ayther_core_version() << " != "
                  << AYTHER_CORE_C_ABI_REVISION << '\n';
        return 1;
    }

    if (ayther_manifest_schema_supported() != AYTHER_PACK_MANIFEST_SCHEMA ||
        ayther_pack_format_supported() != AYTHER_PACK_FORMAT) {
        std::cerr << "pack schema/format contract disagrees with the header\n";
        return 1;
    }

    const char* engine_version = ayther_engine_version();
    if (engine_version == nullptr ||
        std::strcmp(engine_version, AYTHER_VERSION_STRING) != 0) {
        std::cerr << "engine version disagrees with the installed header: "
                  << (engine_version ? engine_version : "(null)") << " != "
                  << AYTHER_VERSION_STRING << '\n';
        return 1;
    }

    std::cout << "AYTHER core " << engine_version
              << " (C ABI revision " << ayther_core_version() << ")\n";
    return 0;
}
