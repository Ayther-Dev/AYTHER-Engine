# AYTHER Engine — standalone root build.
cmake_minimum_required(VERSION 3.21)

# These options precede project() because renderer-related selections may be
# consumed by a vcpkg toolchain while CMake enables the project languages.
option(AYTHER_BUILD_TESTS     "Build the AYTHER test suites"          ON)
option(AYTHER_BUILD_EXAMPLES  "Build the AYTHER examples"             ON)
option(AYTHER_BUILD_RENDERER  "Build the Vulkan renderer"             OFF)
option(AYTHER_BUILD_GPU_TESTS "Build tests that require a Vulkan GPU" OFF)
option(AYTHER_ENABLE_VPX      "Enable VPX video decoding"             OFF)
option(AYTHER_CARGO_LOCKED    "Pass --locked to Cargo"                ON)

if(AYTHER_BUILD_RENDERER OR AYTHER_BUILD_GPU_TESTS)
    list(APPEND VCPKG_MANIFEST_FEATURES "renderer")
endif()

project(Ayther
    VERSION      0.1.0
    DESCRIPTION  "AYTHER Engine - retro asset identity and substitution"
    HOMEPAGE_URL "https://github.com/Ayther-Dev/AYTHER-Engine"
    LANGUAGES    C CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR
        "In-source builds are not supported. Configure with a preset; shared "
        "presets place their output under build/<preset>.")
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(GNUInstallDirs)

set(CMAKE_C_STANDARD            11)
set(CMAKE_C_STANDARD_REQUIRED   ON)
set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(WARNING
        "AYTHER uses Clang/LLVM for its native builds. Current compiler: "
        "${CMAKE_CXX_COMPILER_ID}")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

get_property(AYTHER_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT AYTHER_MULTI_CONFIG AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")

# Cargo is authoritative for the Rust workspace. Corrosion is the only adapter
# between Cargo and the CMake target graph.
include(FetchContent)
FetchContent_Declare(
    Corrosion
    GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
    GIT_TAG        v0.5.1
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(Corrosion)

set(_ayther_corrosion_options "")
if(AYTHER_CARGO_LOCKED)
    list(APPEND _ayther_corrosion_options LOCKED)
endif()

corrosion_import_crate(
    MANIFEST_PATH "${PROJECT_SOURCE_DIR}/Cargo.toml"
    CRATES        ayther_core
    ${_ayther_corrosion_options})
unset(_ayther_corrosion_options)

corrosion_add_cxxbridge(ayther_cxx
    CRATE ayther_core
    FILES ffi.rs)

target_include_directories(ayther_core INTERFACE
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
add_library(Ayther::core ALIAS ayther_core)

# The C++ source and shader trees already live at the repository root. They are
# deliberately not exposed as Ayther::engine until all declared native
# dependencies are present and the complete target can pass its link oracles.
if(EXISTS "${PROJECT_SOURCE_DIR}/src/ayther_session.cpp")
    set(AYTHER_ENGINE_SOURCES_PRESENT ON)
else()
    set(AYTHER_ENGINE_SOURCES_PRESENT OFF)
endif()

if(AYTHER_BUILD_RENDERER)
    message(WARNING
        "AYTHER_BUILD_RENDERER is ON, but the native engine target is not "
        "integrated yet; no renderer target will be produced.")
endif()

if(AYTHER_BUILD_GPU_TESTS)
    message(WARNING
        "AYTHER_BUILD_GPU_TESTS is ON, but GPU test targets are not integrated "
        "yet; no GPU tests will be produced.")
endif()

if(AYTHER_ENABLE_VPX)
    message(WARNING
        "AYTHER_ENABLE_VPX is ON, but the native video target is not integrated "
        "yet; VPX decoding will not be enabled.")
endif()

if(AYTHER_BUILD_EXAMPLES)
    if(EXISTS "${PROJECT_SOURCE_DIR}/examples/CMakeLists.txt")
        add_subdirectory(examples)
    else()
        message(STATUS "AYTHER: no examples/ directory in this checkout; skipping.")
    endif()
endif()

if(AYTHER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

include(AytherInstall)

message(STATUS "AYTHER ${PROJECT_VERSION} configuration")
message(STATUS "  build type ............ ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ compiler .......... ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  tests ................. ${AYTHER_BUILD_TESTS}")
message(STATUS "  examples .............. ${AYTHER_BUILD_EXAMPLES}")
message(STATUS "  renderer .............. ${AYTHER_BUILD_RENDERER}")
message(STATUS "  gpu tests ............. ${AYTHER_BUILD_GPU_TESTS}")
message(STATUS "  VPX decoding .......... ${AYTHER_ENABLE_VPX}")
message(STATUS "  C++ sources present ... ${AYTHER_ENGINE_SOURCES_PRESENT}")
message(STATUS "  installed targets ..... Ayther::core")
