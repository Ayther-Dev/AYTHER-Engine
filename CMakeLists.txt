# AYTHER Engine — standalone root build.
cmake_minimum_required(VERSION 3.21)

# These options precede project() because renderer-related selections may be
# consumed by a vcpkg toolchain while CMake enables the project languages.
option(AYTHER_BUILD_TESTS     "Build the AYTHER test suites"          ON)
option(AYTHER_BUILD_ENGINE    "Build the native C++ engine"           OFF)
option(AYTHER_BUILD_RENDERER  "Build the Vulkan renderer"             OFF)
option(AYTHER_BUILD_GPU_TESTS "Build tests that require a Vulkan GPU" OFF)
option(AYTHER_ENABLE_VPX      "Enable VPX video decoding"             OFF)
option(AYTHER_CARGO_LOCKED    "Pass --locked to Cargo"                ON)

if(AYTHER_BUILD_ENGINE OR AYTHER_BUILD_RENDERER OR AYTHER_BUILD_GPU_TESTS)
    list(APPEND VCPKG_MANIFEST_FEATURES "renderer")
endif()

project(Ayther
    VERSION      0.1.0
    DESCRIPTION  "AYTHER Engine - retro asset identity and substitution"
    HOMEPAGE_URL "https://github.com/Ayther-Dev/AYTHER-Engine"
    LANGUAGES    C CXX)

# The package-manager manifest is another published release-version surface.
# Reject drift during configure instead of shipping contradictory metadata.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg.json" _ayther_vcpkg_manifest)
string(JSON _ayther_vcpkg_version GET "${_ayther_vcpkg_manifest}" version-semver)
if(NOT _ayther_vcpkg_version STREQUAL PROJECT_VERSION)
    message(FATAL_ERROR
        "Version contract mismatch: CMake=${PROJECT_VERSION}, "
        "vcpkg=${_ayther_vcpkg_version}")
endif()
unset(_ayther_vcpkg_manifest)
unset(_ayther_vcpkg_version)

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

# Vendored Yamaha FM synthesis cores used by the voice router.
add_library(ayther_ymfm STATIC EXCLUDE_FROM_ALL
    third_party/ymfm/src/ymfm_adpcm.cpp
    third_party/ymfm/src/ymfm_misc.cpp
    third_party/ymfm/src/ymfm_opl.cpp
    third_party/ymfm/src/ymfm_opm.cpp
    third_party/ymfm/src/ymfm_opn.cpp
    third_party/ymfm/src/ymfm_opq.cpp
    third_party/ymfm/src/ymfm_opz.cpp
    third_party/ymfm/src/ymfm_pcm.cpp
    third_party/ymfm/src/ymfm_ssg.cpp)

target_include_directories(ayther_ymfm SYSTEM PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third_party/ymfm/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/ayther/third_party/ymfm>)

add_library(ayther::ymfm ALIAS ayther_ymfm)
add_library(Ayther::ymfm ALIAS ayther_ymfm)

# tools/build_libvpx.ps1 installs the optional VP9 decoder here. Keep the
# imported target conditional so a checkout without local third-party binaries
# can still configure and build the engine without video decoding.
set(AYTHER_VPX_ROOT "${PROJECT_SOURCE_DIR}/third_party/libvpx" CACHE PATH
    "Root of a locally built libvpx installation")
set(_ayther_vpx_include_dir "${AYTHER_VPX_ROOT}/include")
if(WIN32)
    set(_ayther_vpx_library "${AYTHER_VPX_ROOT}/lib/vpxmd.lib")
else()
    set(_ayther_vpx_library "${AYTHER_VPX_ROOT}/lib/libvpx.a")
endif()

if(EXISTS "${_ayther_vpx_include_dir}/vpx/vpx_decoder.h" AND
   EXISTS "${_ayther_vpx_include_dir}/vpx/vp8dx.h" AND
   EXISTS "${_ayther_vpx_library}")
    add_library(ayther_vpx STATIC IMPORTED GLOBAL)
    set_target_properties(ayther_vpx PROPERTIES
        IMPORTED_LOCATION             "${_ayther_vpx_library}"
        INTERFACE_INCLUDE_DIRECTORIES "${_ayther_vpx_include_dir}")
    add_library(ayther::vpx ALIAS ayther_vpx)
    set(AYTHER_VPX_TARGET_AVAILABLE ON)
    set(AYTHER_VPX_BUNDLED ON)
else()
    set(AYTHER_VPX_TARGET_AVAILABLE OFF)
    set(AYTHER_VPX_BUNDLED OFF)
endif()

unset(_ayther_vpx_include_dir)

# Native C++ engine. Keep the source list explicit so additions and removals are
# visible in review and CMake regenerates only when the build graph changes.
add_library(ayther_engine STATIC EXCLUDE_FROM_ALL
    src/audio_player.cpp
    src/ayther_animation.cpp
    src/ayther_audio_events.cpp
    src/ayther_background_export.cpp
    src/ayther_components_toml.cpp
    src/ayther_config.cpp
    src/ayther_mode3.cpp
    src/ayther_recording.cpp
    src/ayther_renderer.cpp
    src/ayther_sdk.cpp
    src/ayther_sdk_version.cpp
    src/ayther_session.cpp
    src/ayther_session_impl.inl
    src/ayther_video.cpp
    src/psg_synth.cpp
    src/retro_runner.cpp
    src/rewind_buffer.cpp
    src/voice_router.cpp
    src/vulkan_backend/tile_tex_cache.cpp
    src/vulkan_backend/vk_context.cpp
    src/vulkan_backend/vk_indexed_plane.cpp
    src/vulkan_backend/vk_render_target.cpp
    src/vulkan_backend/vk_sprite.cpp
    src/vulkan_backend/vk_texture.cpp
    src/vulkan_backend/vma_impl.cpp)

set_source_files_properties(src/ayther_session_impl.inl
    PROPERTIES HEADER_FILE_ONLY TRUE)

# Installed engine headers are an explicit compatibility surface. Keep this
# allowlist deliberately small: implementation components remain available to
# in-tree tools through include/ayther, but are not published by the package.
set(AYTHER_ENGINE_PUBLIC_HEADER_NAMES
    audio_asset_level.h
    audio_match_rule.h
    ayther_animation.h
    ayther_audio_events.h
    ayther_layers.h
    ayther_mode3.h
    ayther_result.h
    ayther_sdk.h
    ayther_sdk_version.h
    ayther_session.h)

set(AYTHER_ENGINE_PUBLIC_HEADERS "${AYTHER_ENGINE_PUBLIC_HEADER_NAMES}")
list(TRANSFORM AYTHER_ENGINE_PUBLIC_HEADERS
    PREPEND "${PROJECT_SOURCE_DIR}/include/ayther/")

set(AYTHER_INSTALLED_PUBLIC_HEADER_NAMES
    ayther_core_ffi.h
    ayther_version.h
    ${AYTHER_ENGINE_PUBLIC_HEADER_NAMES})

set_source_files_properties(${AYTHER_ENGINE_PUBLIC_HEADERS}
    PROPERTIES HEADER_FILE_ONLY TRUE)
source_group("Public Headers" FILES ${AYTHER_ENGINE_PUBLIC_HEADERS})
target_sources(ayther_engine PRIVATE ${AYTHER_ENGINE_PUBLIC_HEADERS})

set(AYTHER_SHADER_SOURCES
    "${PROJECT_SOURCE_DIR}/shaders/indexed_plane.frag"
    "${PROJECT_SOURCE_DIR}/shaders/indexed_plane.vert"
    "${PROJECT_SOURCE_DIR}/shaders/sprite.frag"
    "${PROJECT_SOURCE_DIR}/shaders/sprite.vert"
    "${PROJECT_SOURCE_DIR}/shaders/sprite_mask.frag"
    "${PROJECT_SOURCE_DIR}/shaders/sprite_mult.frag"
    "${PROJECT_SOURCE_DIR}/shaders/sprite_screen.frag"
    "${PROJECT_SOURCE_DIR}/shaders/video.frag")

set(AYTHER_SHADER_BINARIES
    "${PROJECT_SOURCE_DIR}/shaders/indexed_plane.frag.spv"
    "${PROJECT_SOURCE_DIR}/shaders/indexed_plane.vert.spv"
    "${PROJECT_SOURCE_DIR}/shaders/sprite.frag.spv"
    "${PROJECT_SOURCE_DIR}/shaders/sprite.vert.spv"
    "${PROJECT_SOURCE_DIR}/shaders/sprite_mask.frag.spv"
    "${PROJECT_SOURCE_DIR}/shaders/sprite_mult.frag.spv"
    "${PROJECT_SOURCE_DIR}/shaders/sprite_screen.frag.spv"
    "${PROJECT_SOURCE_DIR}/shaders/video.frag.spv")

set(AYTHER_SHADER_RESOURCES
    ${AYTHER_SHADER_SOURCES}
    ${AYTHER_SHADER_BINARIES})

set_source_files_properties(${AYTHER_SHADER_RESOURCES}
    PROPERTIES HEADER_FILE_ONLY TRUE)
source_group(TREE "${PROJECT_SOURCE_DIR}/shaders"
    PREFIX "Shaders"
    FILES ${AYTHER_SHADER_RESOURCES})
target_sources(ayther_engine PRIVATE ${AYTHER_SHADER_RESOURCES})

target_include_directories(ayther_engine
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        "${PROJECT_SOURCE_DIR}/include/ayther"
        "${PROJECT_SOURCE_DIR}/src")

target_compile_features(ayther_engine PUBLIC cxx_std_20)

set_target_properties(ayther_engine PROPERTIES
    EXPORT_NAME   engine
    PUBLIC_HEADER "${AYTHER_ENGINE_PUBLIC_HEADERS}")
set_target_properties(ayther_ymfm PROPERTIES EXPORT_NAME ymfm)

add_library(Ayther::engine ALIAS ayther_engine)

set(AYTHER_ENGINE_SOURCES_PRESENT ON)

if(AYTHER_BUILD_ENGINE)
    if(NOT AYTHER_BUILD_RENDERER)
        message(FATAL_ERROR
            "AYTHER_BUILD_ENGINE requires AYTHER_BUILD_RENDERER because the "
            "complete 24-source engine target includes SDL3/Vulkan backends.")
    endif()

    find_package(Threads REQUIRED)
    find_package(SDL3 CONFIG REQUIRED)
    find_package(Vulkan REQUIRED)
    find_package(VulkanMemoryAllocator CONFIG REQUIRED)
    find_package(vk-bootstrap CONFIG REQUIRED)
    find_package(tomlplusplus CONFIG REQUIRED)
    find_package(Stb REQUIRED)
    find_package(zstd CONFIG REQUIRED)
    find_path(AYTHER_DRLIBS_INCLUDE_DIR NAMES dr_flac.h REQUIRED)

    # The vcpkg ports for stb and dr_libs expose include-directory variables
    # rather than imported targets. Normalize them here so every native
    # dependency participates in the target graph and no dependency include
    # path is attached directly to ayther_engine.
    if(NOT TARGET Stb::Stb)
        add_library(Stb::Stb INTERFACE IMPORTED)
        set_target_properties(Stb::Stb PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${Stb_INCLUDE_DIR}")
    endif()

    if(NOT TARGET dr_libs::dr_libs)
        add_library(dr_libs::dr_libs INTERFACE IMPORTED)
        set_target_properties(dr_libs::dr_libs PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${AYTHER_DRLIBS_INCLUDE_DIR}")
    endif()

    set(_ayther_native_dependency_targets
        SDL3::SDL3
        Vulkan::Vulkan
        GPUOpen::VulkanMemoryAllocator
        vk-bootstrap::vk-bootstrap
        Stb::Stb
        dr_libs::dr_libs
        tomlplusplus::tomlplusplus
        zstd::libzstd)

    foreach(_ayther_dependency_target IN LISTS
            _ayther_native_dependency_targets)
        if(NOT TARGET "${_ayther_dependency_target}")
            message(FATAL_ERROR
                "Native dependency did not provide the required CMake target: "
                "${_ayther_dependency_target}")
        endif()
    endforeach()

    if(AYTHER_ENABLE_VPX AND NOT TARGET ayther::vpx)
        if(WIN32)
            message(FATAL_ERROR
                "AYTHER_ENABLE_VPX requires ${AYTHER_VPX_ROOT}/lib/vpxmd.lib "
                "and matching headers. Run tools/build_libvpx.ps1 first.")
        else()
            find_package(PkgConfig REQUIRED)
            pkg_check_modules(AYTHER_SYSTEM_VPX REQUIRED IMPORTED_TARGET vpx)
            add_library(ayther_vpx INTERFACE)
            target_link_libraries(ayther_vpx INTERFACE PkgConfig::AYTHER_SYSTEM_VPX)
            add_library(ayther::vpx ALIAS ayther_vpx)
            set(AYTHER_VPX_TARGET_AVAILABLE ON)
        endif()
    endif()

    target_compile_definitions(ayther_engine PRIVATE
        VMA_STATIC_VULKAN_FUNCTIONS=1
        VMA_DYNAMIC_VULKAN_FUNCTIONS=0
        $<$<BOOL:${AYTHER_ENABLE_VPX}>:AYTHER_HAVE_VPX=1>)

    target_link_libraries(ayther_engine
        PUBLIC
            # In-tree tools compile implementation headers directly. Preserve
            # their dependency includes only in the build tree; the installed
            # allowlist exposes neither SDL nor Vulkan types.
            $<BUILD_INTERFACE:SDL3::SDL3>
            $<BUILD_INTERFACE:Vulkan::Vulkan>
        PRIVATE
            $<BUILD_INTERFACE:Ayther::core>
            $<BUILD_INTERFACE:ayther::ymfm>
            $<INSTALL_INTERFACE:Ayther::ymfm>
            Threads::Threads
            SDL3::SDL3
            Vulkan::Vulkan
            GPUOpen::VulkanMemoryAllocator
            vk-bootstrap::vk-bootstrap
            $<BUILD_INTERFACE:Stb::Stb>
            $<BUILD_INTERFACE:dr_libs::dr_libs>
            tomlplusplus::tomlplusplus
            zstd::libzstd
            ${CMAKE_DL_LIBS})

    if(AYTHER_ENABLE_VPX)
        target_link_libraries(ayther_engine PRIVATE
            $<BUILD_INTERFACE:ayther::vpx>
            $<INSTALL_INTERFACE:Ayther::vpx>)
    endif()

    set_target_properties(ayther_engine PROPERTIES EXCLUDE_FROM_ALL FALSE)
endif()

unset(_ayther_dependency_target)
unset(_ayther_native_dependency_targets)

if(AYTHER_BUILD_GPU_TESTS AND NOT AYTHER_BUILD_ENGINE)
    message(FATAL_ERROR
        "AYTHER_BUILD_GPU_TESTS requires AYTHER_BUILD_ENGINE=ON.")
endif()

if(AYTHER_BUILD_GPU_TESTS AND NOT AYTHER_BUILD_TESTS)
    message(FATAL_ERROR
        "AYTHER_BUILD_GPU_TESTS requires AYTHER_BUILD_TESTS=ON.")
endif()

if(AYTHER_ENABLE_VPX AND NOT AYTHER_BUILD_ENGINE)
    message(FATAL_ERROR "AYTHER_ENABLE_VPX requires AYTHER_BUILD_ENGINE=ON.")
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
message(STATUS "  engine ................ ${AYTHER_BUILD_ENGINE}")
message(STATUS "  renderer .............. ${AYTHER_BUILD_RENDERER}")
message(STATUS "  gpu tests ............. ${AYTHER_BUILD_GPU_TESTS}")
message(STATUS "  VPX decoding .......... ${AYTHER_ENABLE_VPX}")
message(STATUS "  libvpx target ......... ${AYTHER_VPX_TARGET_AVAILABLE}")
message(STATUS "  C++ sources present ... ${AYTHER_ENGINE_SOURCES_PRESENT}")
if(AYTHER_BUILD_ENGINE)
    message(STATUS "  installed targets ..... Ayther::core, Ayther::engine")
else()
    message(STATUS "  installed targets ..... Ayther::core")
endif()
