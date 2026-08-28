#pragma once
// ---------------------------------------------------------------------------
// Canonical AYTHER release and compatibility version contract.
//
// AYTHER_VERSION_* identifies the product release shared by Cargo, CMake, the
// native SDK, the engine compatibility check, and the Lua API. The remaining
// values are independent protocol revisions and do not follow SemVer.
// ---------------------------------------------------------------------------

#define AYTHER_VERSION_MAJOR 0
#define AYTHER_VERSION_MINOR 1
#define AYTHER_VERSION_PATCH 0

#define AYTHER_VERSION_STRINGIFY_(x) #x
#define AYTHER_VERSION_STRINGIFY(x) AYTHER_VERSION_STRINGIFY_(x)
#define AYTHER_VERSION_STRING                                      \
    AYTHER_VERSION_STRINGIFY(AYTHER_VERSION_MAJOR) "."             \
    AYTHER_VERSION_STRINGIFY(AYTHER_VERSION_MINOR) "."             \
    AYTHER_VERSION_STRINGIFY(AYTHER_VERSION_PATCH)

/// Revision of the legacy flat C ABI exported by ayther_core.
#define AYTHER_CORE_C_ABI_REVISION 5

/// Latest .ay pack manifest schema written and understood by this release.
#define AYTHER_PACK_MANIFEST_SCHEMA 2
