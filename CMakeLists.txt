# ---------------------------------------------------------------------------
# AYTHER Engine - root build.
#
# This project builds the Rust `ayther_core` static library through cargo,
# publishes it as the `Ayther::core` target together with the public C header
# in `include/`, and adds the C++ suites that exercise that boundary.
#
# Configure through the shared presets rather than by hand:
#
#     cmake --preset windows-headless
#     cmake --build --preset windows-headless
#     ctest --preset windows-headless
#
# See docs/DEVELOPMENT_ENVIRONMENT.md for toolchain prerequisites and
# docs/PROJECT_STATUS.md for what this repository does and does not contain.
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.21)

# ---------------------------------------------------------------------------
# Options.
#
# Declared before project() because AYTHER_BUILD_RENDERER selects the vcpkg
# manifest features, and the vcpkg toolchain file reads VCPKG_MANIFEST_FEATURES
# while project() runs.
# ---------------------------------------------------------------------------
option(AYTHER_BUILD_TESTS     "Build the AYTHER test suites"           ON)
option(AYTHER_BUILD_EXAMPLES  "Build the AYTHER examples"              ON)
option(AYTHER_BUILD_RENDERER  "Build the Vulkan renderer"              OFF)
option(AYTHER_BUILD_GPU_TESTS "Build tests that require a Vulkan GPU"  OFF)
option(AYTHER_ENABLE_VPX      "Enable VPX video decoding"              OFF)
option(AYTHER_CARGO_LOCKED    "Pass --locked to cargo (reproducible)"  ON)

if(AYTHER_BUILD_RENDERER OR AYTHER_BUILD_GPU_TESTS)
    list(APPEND VCPKG_MANIFEST_FEATURES "renderer")
endif()

project(Ayther
    VERSION     0.1.0
    DESCRIPTION "AYTHER Engine - retro asset identity and substitution"
    HOMEPAGE_URL "https://github.com/Ayther-Dev/AYTHER-Engine"
    LANGUAGES   C CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR
        "In-source builds are not supported. Configure with a preset, which "
        "places each build under build/<preset>.")
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(GNUInstallDirs)

# ---------------------------------------------------------------------------
# Language and runtime settings.
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD            11)
set(CMAKE_C_STANDARD_REQUIRED   ON)
set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

# Rust MSVC targets always link the dynamic release CRT (msvcrt), so the C++
# half must do the same or the two halves pull in conflicting runtimes.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

get_property(AYTHER_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT AYTHER_MULTI_CONFIG AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")

# ---------------------------------------------------------------------------
# ayther_core: the Rust static library plus the public C header.
# ---------------------------------------------------------------------------
include(AytherCargo)
ayther_add_rust_core()

# ---------------------------------------------------------------------------
# Components that have not been migrated into this repository yet.
# See docs/PROJECT_STATUS.md, "Release blockers".
# ---------------------------------------------------------------------------
if(AYTHER_BUILD_RENDERER)
    message(WARNING
        "AYTHER_BUILD_RENDERER is ON but no engine/ directory exists in this "
        "checkout, so no renderer target is produced. Use a headless preset.")
endif()

if(AYTHER_ENABLE_VPX)
    message(WARNING
        "AYTHER_ENABLE_VPX is ON but no video decoding target exists in this "
        "checkout; the option has no effect.")
endif()

if(AYTHER_BUILD_EXAMPLES)
    if(EXISTS "${PROJECT_SOURCE_DIR}/examples/CMakeLists.txt")
        add_subdirectory(examples)
    else()
        message(STATUS "AYTHER: no examples/ directory in this checkout; skipping.")
    endif()
endif()

# ---------------------------------------------------------------------------
# Tests.
# ---------------------------------------------------------------------------
if(AYTHER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()

# ---------------------------------------------------------------------------
# Install and package.
#
# Installs the cargo-produced archive and the public header, and generates an
# AytherConfig.cmake that recreates Ayther::core for out-of-tree consumers.
# ---------------------------------------------------------------------------
include(CMakePackageConfigHelpers)

install(FILES "${PROJECT_SOURCE_DIR}/include/ayther_core_ffi.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

install(FILES "${AYTHER_CORE_STATIC_LIB}"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}")

set(AYTHER_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/Ayther")

configure_package_config_file(
    "${PROJECT_SOURCE_DIR}/cmake/AytherConfig.cmake.in"
    "${PROJECT_BINARY_DIR}/AytherConfig.cmake"
    INSTALL_DESTINATION "${AYTHER_INSTALL_CMAKEDIR}"
    PATH_VARS CMAKE_INSTALL_INCLUDEDIR CMAKE_INSTALL_LIBDIR)

write_basic_package_version_file(
    "${PROJECT_BINARY_DIR}/AytherConfigVersion.cmake"
    VERSION       ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
            "${PROJECT_BINARY_DIR}/AytherConfig.cmake"
            "${PROJECT_BINARY_DIR}/AytherConfigVersion.cmake"
        DESTINATION "${AYTHER_INSTALL_CMAKEDIR}")

# ---------------------------------------------------------------------------
# Configuration summary.
# ---------------------------------------------------------------------------
message(STATUS "AYTHER ${PROJECT_VERSION} configuration")
message(STATUS "  build type ....... ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ compiler ..... ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  cargo ............ ${AYTHER_CARGO_EXECUTABLE}")
message(STATUS "  cargo profile .... ${AYTHER_CARGO_PROFILE_DIR}")
message(STATUS "  tests ............ ${AYTHER_BUILD_TESTS}")
message(STATUS "  examples ......... ${AYTHER_BUILD_EXAMPLES}")
message(STATUS "  renderer ......... ${AYTHER_BUILD_RENDERER}")
message(STATUS "  gpu tests ........ ${AYTHER_BUILD_GPU_TESTS}")
