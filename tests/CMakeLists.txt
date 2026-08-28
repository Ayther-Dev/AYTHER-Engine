# ---------------------------------------------------------------------------
# C++ suites that exercise the ayther_core boundary.
#
# These link only against Ayther::core, so they build and run under the
# headless presets with no SDL3, Vulkan, or GPU present.
# ---------------------------------------------------------------------------

add_executable(ayther_core_ffi_tests ayther_core_ffi_test.cpp)

target_link_libraries(ayther_core_ffi_tests PRIVATE Ayther::core)

if(MSVC)
    target_compile_options(ayther_core_ffi_tests PRIVATE /W4)
else()
    target_compile_options(ayther_core_ffi_tests PRIVATE -Wall -Wextra)
endif()

add_test(NAME ayther.core.ffi COMMAND ayther_core_ffi_tests)

set_tests_properties(ayther.core.ffi PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    TIMEOUT           120
    LABELS            "core;abi;headless")

# The SF2 boundary smoke only exercises Ayther::core and remains available in
# headless configurations. Its no-argument mode validates the null-handle
# contract without requiring a user-provided SoundFont.
add_subdirectory(
    "${PROJECT_SOURCE_DIR}/tools/sf2_synth_smoke"
    "${PROJECT_BINARY_DIR}/tools/sf2_synth_smoke")

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
    ayther_sdk_version.h)
add_test(
    NAME ayther.quality.documentation_language
    COMMAND "${CMAKE_COMMAND}"
        "-DAYTHER_INCLUDE_DIR=${PROJECT_SOURCE_DIR}/include/ayther"
        "-DAYTHER_DOCUMENTED_HEADERS=${_ayther_english_documented_headers}"
        -P "${PROJECT_SOURCE_DIR}/tools/check_documentation_language.cmake")
set_tests_properties(ayther.quality.documentation_language PROPERTIES
    TIMEOUT 30
    LABELS  "engine;native;quality")

# Native unit/contract tests are standalone executables with a tiny local test
# harness. Give each one an isolated working directory because several create
# temporary WAV, pack, or recording files using fixed names.
set(_ayther_native_test_sources
    audio_asset_fallback_test.cpp
    audio_bus_balance_test.cpp
    audio_fade_test.cpp
    audio_level_test.cpp
    audio_live_resume_test.cpp
    audio_match_identity_test.cpp
    audio_match_rule_test.cpp
    audio_player_pause_test.cpp
    audio_seq_anchor_test.cpp
    audio_tail_test.cpp
    audio_unified_mix_test.cpp
    ayther_recording_test.cpp
    cram_palette_test.cpp
    elements_toml_test.cpp
    failure_escalation_test.cpp
    gain_source_test.cpp
    loop_region_test.cpp
    output_profile_test.cpp
    pano_bands_test.cpp
    panorama_cover_test.cpp
    parallax_bands_test.cpp
    plane_hash_variants_test.cpp
    video_bt601_test.cpp
    widescreen_test.cpp
    xbr_ref_test.cpp)

foreach(_ayther_native_test_source IN LISTS _ayther_native_test_sources)
    get_filename_component(_ayther_native_test_name
        "${_ayther_native_test_source}" NAME_WE)
    set(_ayther_native_test_target
        "ayther_${_ayther_native_test_name}")
    set(_ayther_native_test_workdir
        "${CMAKE_CURRENT_BINARY_DIR}/${_ayther_native_test_name}")

    add_executable("${_ayther_native_test_target}"
        "${_ayther_native_test_source}")
    target_link_libraries("${_ayther_native_test_target}" PRIVATE
        Ayther::engine
        Ayther::core)
    target_include_directories("${_ayther_native_test_target}" PRIVATE
        "${PROJECT_SOURCE_DIR}/include/ayther")
    target_compile_features("${_ayther_native_test_target}" PRIVATE
        cxx_std_20)

    if(MSVC)
        target_compile_options("${_ayther_native_test_target}" PRIVATE /W4)
    else()
        target_compile_options("${_ayther_native_test_target}" PRIVATE
            -Wall -Wextra)
    endif()

    file(MAKE_DIRECTORY "${_ayther_native_test_workdir}")
    add_test(
        NAME "ayther.native.${_ayther_native_test_name}"
        COMMAND "${_ayther_native_test_target}")
    set_tests_properties(
        "ayther.native.${_ayther_native_test_name}"
        PROPERTIES
            WORKING_DIRECTORY "${_ayther_native_test_workdir}"
            TIMEOUT           120
            LABELS            "engine;native;unit")
endforeach()

# Pass optional local core/ROM paths to ABI probes without making them required
# on clean checkouts. The executables report CTest's conventional skip code 77
# when the external emulator core is unavailable.
function(ayther_abi_test_env out_var)
    set(_ayther_env "")
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

set(_ayther_native_tool_tests
    abi_frame_delta
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
endforeach()

if(AYTHER_ENABLE_VPX)
    foreach(_ayther_vpx_test IN ITEMS
            video_decode_smoke video_stream_smoke vpx_smoke)
        add_subdirectory(
            "${PROJECT_SOURCE_DIR}/tools/${_ayther_vpx_test}"
            "${PROJECT_BINARY_DIR}/tools/${_ayther_vpx_test}")
        target_include_directories("${_ayther_vpx_test}" PRIVATE
            "${PROJECT_SOURCE_DIR}/include/ayther")
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
    endforeach()
endif()

unset(_ayther_gpu_test)
unset(_ayther_native_test_name)
unset(_ayther_native_test_source)
unset(_ayther_native_test_sources)
unset(_ayther_native_test_target)
unset(_ayther_native_test_workdir)
unset(_ayther_native_tool_tests)
unset(_ayther_tool_test)
unset(_ayther_vpx_test)
