# ---------------------------------------------------------------------------
# Test suites, grouped by what they exercise:
#
#   ffi/          the ayther_core ABI and handle lifecycle
#   unit/         pure logic and header-only components
#   integration/  session, audio, recording, and pack documents
#   renderer/     Vulkan and shader contracts
#
# Only ffi/ builds in headless configurations; the rest need Ayther::engine.
# Each group's CTest names and labels carry its directory, so `ctest -L unit`
# and friends select exactly one group.
# ---------------------------------------------------------------------------

# Native unit/contract tests are standalone executables with a tiny local test
# harness. Give each one an isolated working directory because several create
# temporary WAV, pack, or recording files using fixed names.
add_library(ayther_test_support INTERFACE)
target_include_directories(ayther_test_support INTERFACE
    "${PROJECT_SOURCE_DIR}/tests/support")

# Every Runtime-facing public header must compile first in an otherwise empty
# translation unit. Object targets make this a build gate without requiring the
# native Engine or any renderer dependency.
foreach(_header_probe IN ITEMS
        capabilities_header
        core_probe_header
        engine_header)
    add_library("ayther_${_header_probe}" OBJECT
        "${PROJECT_SOURCE_DIR}/tests/contracts/${_header_probe}.cpp")
    target_include_directories("ayther_${_header_probe}" PRIVATE
        "${PROJECT_SOURCE_DIR}/include")
    target_compile_features("ayther_${_header_probe}" PRIVATE cxx_std_20)
    ayther_instrument_target("ayther_${_header_probe}")
endforeach()

add_test(
    NAME ayther.package.engine_public_contract
    COMMAND "${CMAKE_COMMAND}"
        "-DAYTHER_ENGINE_PUBLIC_DIR=${PROJECT_SOURCE_DIR}/include/ayther/engine"
        "-DAYTHER_ENGINE_PUBLIC_HEADERS=capabilities.hpp;core_probe.hpp;engine.hpp"
        -P "${PROJECT_SOURCE_DIR}/tools/check_engine_public_contract.cmake")
set_tests_properties(ayther.package.engine_public_contract PROPERTIES
    TIMEOUT 30
    LABELS  "native;package")

function(ayther_add_native_test _source _category)
    get_filename_component(_name "${_source}" NAME_WE)
    set(_target "ayther_${_name}")
    set(_workdir "${CMAKE_CURRENT_BINARY_DIR}/${_name}")

    add_executable("${_target}" "${_source}")
    target_link_libraries("${_target}" PRIVATE
        Ayther::engine
        Ayther::core
        ayther_test_support)
    target_include_directories("${_target}" PRIVATE
        "${PROJECT_SOURCE_DIR}/include/ayther"
        "${PROJECT_SOURCE_DIR}/src")
    target_compile_features("${_target}" PRIVATE cxx_std_20)
    ayther_instrument_target("${_target}")

    if(MSVC)
        target_compile_options("${_target}" PRIVATE /W4)
    else()
        target_compile_options("${_target}" PRIVATE -Wall -Wextra)
    endif()

    file(MAKE_DIRECTORY "${_workdir}")
    add_test(NAME "ayther.${_category}.${_name}" COMMAND "${_target}")
    set_tests_properties("ayther.${_category}.${_name}" PROPERTIES
        WORKING_DIRECTORY "${_workdir}"
        TIMEOUT           120
        LABELS            "engine;native;${_category}")
endfunction()

# The ABI suite links only Ayther::core, so it builds and runs under the
# headless presets with no SDL3, Vulkan, or GPU present.
add_subdirectory(ffi)

# The SF2 boundary smoke only exercises Ayther::core and remains available in
# headless configurations. Its no-argument mode validates the null-handle
# contract without requiring a user-provided SoundFont.
add_subdirectory(
    "${PROJECT_SOURCE_DIR}/tools/sf2_synth_smoke"
    "${PROJECT_BINARY_DIR}/tools/sf2_synth_smoke")
ayther_instrument_target(sf2_synth_smoke)

if(NOT AYTHER_BUILD_ENGINE)
    return()
endif()

# The installation allowlist must be transitively closed: every project header
# included by a public header must itself be public and installed.
add_test(
    NAME ayther.package.public_headers
    COMMAND "${CMAKE_COMMAND}"
        "-DAYTHER_INCLUDE_DIR=${PROJECT_SOURCE_DIR}/include/ayther"
        "-DAYTHER_PUBLIC_HEADERS=${AYTHER_INSTALLED_PUBLIC_HEADER_NAMES}"
        -P "${PROJECT_SOURCE_DIR}/tools/check_public_headers.cmake")
set_tests_properties(ayther.package.public_headers PROPERTIES
    TIMEOUT 30
    LABELS  "engine;native;package")

# Guard the public headers documented during the native-target migration. The
# much larger legacy session/FFI headers are tracked as a separate migration in
# docs/CPP_ENGINE_REVIEW.md and are not silently grandfathered into this list.
set(_ayther_english_documented_headers
    audio_asset_level.h
    audio_match_rule.h
    ayther_animation.h
    ayther_audio_events.h
    ayther_layers.h
    ayther_mode3.h
    ayther_result.h
    ayther_sdk.h
    ayther_sdk_version.h
    ayther_version.h
    engine/core_probe.hpp)
add_test(
    NAME ayther.quality.documentation_language
    COMMAND "${CMAKE_COMMAND}"
        "-DAYTHER_INCLUDE_DIR=${PROJECT_SOURCE_DIR}/include/ayther"
        "-DAYTHER_DOCUMENTED_HEADERS=${_ayther_english_documented_headers}"
        -P "${PROJECT_SOURCE_DIR}/tools/check_documentation_language.cmake")
set_tests_properties(ayther.quality.documentation_language PROPERTIES
    TIMEOUT 30
    LABELS  "engine;native;quality")

add_subdirectory(unit)
add_subdirectory(integration)
add_subdirectory(renderer)

# Pass optional local core/ROM paths to ABI probes without making them required
# on clean checkouts. The executables report CTest's conventional skip code 77
# when the external emulator core is unavailable.
function(ayther_abi_test_env out_var)
    # The in-repo test cores are the default, so the ABI oracles measure the
    # negotiation on every checkout instead of reporting 77. An environment
    # variable still wins: pointing these at a real fork core is how the same
    # oracles get run against the thing they ultimately describe.
    set(_ayther_env
        "AYTHER_ABI_CORE=$<TARGET_FILE:ayther_test_core>"
        "AYTHER_STOCK_CORE=$<TARGET_FILE:ayther_test_core_stock>")
    foreach(_ayther_env_name IN ITEMS
            AYTHER_ABI_CORE AYTHER_STOCK_CORE AYTHER_PROBE_ROM)
        if(DEFINED ENV{${_ayther_env_name}} AND
           NOT "$ENV{${_ayther_env_name}}" STREQUAL "")
            list(APPEND _ayther_env
                "${_ayther_env_name}=$ENV{${_ayther_env_name}}")
        endif()
    endforeach()
    set("${out_var}" "${_ayther_env}" PARENT_SCOPE)
endfunction()

# The ABI oracles need an emulator core that speaks the extension. Building one
# here -- rather than pointing at a binary that /third_party/cores/* gitignores
# -- is what lets the positive half of the negotiation run on a clean checkout.
add_subdirectory(
    "${PROJECT_SOURCE_DIR}/tools/test_core"
    "${PROJECT_BINARY_DIR}/tools/test_core")

# A loadable library without Libretro entry points makes the negative probe
# case deterministic on every platform and independent of system DLL layout.
add_library(ayther_not_a_core MODULE
    "${PROJECT_SOURCE_DIR}/tests/support/not_a_core.cpp")
set_target_properties(ayther_not_a_core PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
target_compile_features(ayther_not_a_core PRIVATE cxx_std_20)
target_link_libraries(ayther_not_a_core PRIVATE
    $<BUILD_INTERFACE:ayther_strict_warnings>)

ayther_add_native_test(
    "${PROJECT_SOURCE_DIR}/tests/integration/core_probe_test.cpp"
    integration)
target_compile_definitions(ayther_core_probe_test PRIVATE
    AYTHER_TEST_CORE_PATH="$<TARGET_FILE:ayther_test_core>"
    AYTHER_NOT_A_CORE_PATH="$<TARGET_FILE:ayther_not_a_core>"
    AYTHER_MISSING_CORE_PATH="${CMAKE_CURRENT_BINARY_DIR}/missing-core")
add_dependencies(ayther_core_probe_test
    ayther_test_core
    ayther_not_a_core)

# Signed content for the release-candidate consumer, which cannot open an
# unsigned pack in an optimized build.
add_subdirectory(
    "${PROJECT_SOURCE_DIR}/tools/make_test_pack"
    "${PROJECT_BINARY_DIR}/tools/make_test_pack")

set(_ayther_native_tool_tests
    abi_frame_delta
    e2e_determinism
    abi_multilayer
    abi_read_parity
    abi_write_control
    audio_mute_smoke
    audio_output_smoke
    ayther_abi_smoke
    render_output_smoke
    subsystem_routing_smoke
    voice_router_smoke)

foreach(_ayther_tool_test IN LISTS _ayther_native_tool_tests)
    add_subdirectory(
        "${PROJECT_SOURCE_DIR}/tools/${_ayther_tool_test}"
        "${PROJECT_BINARY_DIR}/tools/${_ayther_tool_test}")
    target_include_directories("${_ayther_tool_test}" PRIVATE
        "${PROJECT_SOURCE_DIR}/include/ayther")
    ayther_instrument_target("${_ayther_tool_test}")
    # The oracles dlopen the cores rather than linking them, so the dependency
    # is invisible to the linker and has to be declared.
    add_dependencies("${_ayther_tool_test}"
        ayther_test_core ayther_test_core_stock)
endforeach()

if(AYTHER_ENABLE_VPX)
    foreach(_ayther_vpx_test IN ITEMS
            video_decode_smoke video_stream_smoke vpx_smoke)
        add_subdirectory(
            "${PROJECT_SOURCE_DIR}/tools/${_ayther_vpx_test}"
            "${PROJECT_BINARY_DIR}/tools/${_ayther_vpx_test}")
        target_include_directories("${_ayther_vpx_test}" PRIVATE
            "${PROJECT_SOURCE_DIR}/include/ayther")
        target_link_libraries("${_ayther_vpx_test}" PRIVATE ayther_test_support)
        ayther_instrument_target("${_ayther_vpx_test}")
    endforeach()
endif()

if(AYTHER_BUILD_GPU_TESTS)
    foreach(_ayther_gpu_test IN ITEMS
            ab_compare_smoke
            overlay_blend_smoke
            overlay_vscroll_smoke
            enhance_shader_smoke
            sprite_mask_smoke
            video_plane_smoke
            video_shader_smoke
            widescreen_sprites_smoke)
        add_subdirectory(
            "${PROJECT_SOURCE_DIR}/tools/${_ayther_gpu_test}"
            "${PROJECT_BINARY_DIR}/tools/${_ayther_gpu_test}")
        target_include_directories("${_ayther_gpu_test}" PRIVATE
            "${PROJECT_SOURCE_DIR}/include/ayther")
        ayther_instrument_target("${_ayther_gpu_test}")
    endforeach()
endif()

unset(_ayther_gpu_test)
unset(_ayther_native_tool_tests)
unset(_ayther_tool_test)
unset(_ayther_vpx_test)
