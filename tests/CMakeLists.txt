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
        audio_observer_header
        capabilities_header
        core_probe_header
        input_header
        layers_header
        render_observer_header
        visual_state_header
        session_header
        pack_header)
    add_library("ayther_${_header_probe}" OBJECT
        "${PROJECT_SOURCE_DIR}/tests/contracts/${_header_probe}.cpp")
    target_include_directories("ayther_${_header_probe}" PRIVATE
        "${PROJECT_SOURCE_DIR}/include")
    target_compile_features("ayther_${_header_probe}" PRIVATE cxx_std_20)
    ayther_instrument_target("ayther_${_header_probe}")
endforeach()

add_library(ayther_core_ffi_header OBJECT
    "${PROJECT_SOURCE_DIR}/tests/contracts/core_ffi_header.cpp")
target_include_directories(ayther_core_ffi_header PRIVATE
    "${PROJECT_SOURCE_DIR}/include")
target_compile_features(ayther_core_ffi_header PRIVATE cxx_std_20)
ayther_instrument_target(ayther_core_ffi_header)

# The installed Engine umbrella and Vulkan interop surface intentionally expose
# Vulkan native types. They exist only for native Engine artifacts, and linking
# the imported target supplies the same include contract package consumers get.
if(AYTHER_BUILD_ENGINE)
    foreach(_header_probe IN ITEMS
            engine_header
            renderer_header
            vulkan_interop_header)
        add_library("ayther_${_header_probe}" OBJECT
            "${PROJECT_SOURCE_DIR}/tests/contracts/${_header_probe}.cpp")
        target_include_directories("ayther_${_header_probe}" PRIVATE
            "${PROJECT_SOURCE_DIR}/include")
        target_link_libraries("ayther_${_header_probe}" PRIVATE Vulkan::Vulkan)
        target_compile_features("ayther_${_header_probe}" PRIVATE cxx_std_20)
        ayther_instrument_target("ayther_${_header_probe}")
    endforeach()
endif()

add_test(
    NAME ayther.package.engine_public_contract
    COMMAND "${CMAKE_COMMAND}"
        "-DAYTHER_ENGINE_PUBLIC_DIR=${PROJECT_SOURCE_DIR}/include/ayther/engine"
        "-DAYTHER_ENGINE_PUBLIC_HEADERS=audio_fact_queue.hpp;audio_hd_state.hpp;audio_initial_snapshot.hpp;audio_observation_close.hpp;audio_observation_overflow.hpp;audio_observation_worker.hpp;audio_observer.hpp;audio_pcm_queue.hpp;audio_production_limit.hpp;capabilities.hpp;core_probe.hpp;engine.hpp;input.hpp;music_analysis_limits.hpp;music_analysis_snapshot.hpp;music_asset_owner.hpp;music_audio_math.hpp;music_causal_recognizer.hpp;music_corpus_quality.hpp;music_event_normalization.hpp;music_event_scoring.hpp;music_execution_adapter.hpp;music_identity_state.hpp;music_late_entry.hpp;music_live_recognition.hpp;music_pack_contract.hpp;music_pause_signal.hpp;music_pause_state.hpp;music_pattern_analysis.hpp;music_position_recovery.hpp;music_proposal_assessment.hpp;music_recognition_candidate.hpp;music_sequence.hpp;music_sequence_validation.hpp;music_source_lifecycle.hpp;music_transition_arbiter.hpp;music_transition_cancel.hpp;music_transition_queue.hpp;music_transition_span.hpp;music_voice_budget.hpp;pack.hpp;render_observer.hpp;visual_state.hpp;vulkan_interop.hpp"
        -P "${PROJECT_SOURCE_DIR}/tools/check_engine_public_contract.cmake")
set_tests_properties(ayther.package.engine_public_contract PROPERTIES
    TIMEOUT 30
    LABELS  "native;package")

find_program(AYTHER_PWSH_EXECUTABLE NAMES pwsh)
add_subdirectory(audio_qa/observer_contract)
if(AYTHER_PWSH_EXECUTABLE)
    add_test(
        NAME ayther.quality.core_test_results
        COMMAND "${AYTHER_PWSH_EXECUTABLE}" -NoProfile -File
            "${PROJECT_SOURCE_DIR}/tests/core_test_results_test.ps1"
            -Checker "${PROJECT_SOURCE_DIR}/tools/check_core_test_results.ps1"
            -WorkDirectory "${PROJECT_BINARY_DIR}/tests/core_test_results")
    set_tests_properties(ayther.quality.core_test_results PROPERTIES
        TIMEOUT 60 LABELS "quality")
    add_test(
        NAME ayther.package.release_assets
        COMMAND "${AYTHER_PWSH_EXECUTABLE}" -NoProfile -File
            "${PROJECT_SOURCE_DIR}/tests/release_assets_test.ps1"
            -WorkDirectory
            "${PROJECT_BINARY_DIR}/tests/release_assets_contract"
            -Finalizer
            "${PROJECT_SOURCE_DIR}/tools/finalize_release_assets.ps1"
            -Packager
            "${PROJECT_SOURCE_DIR}/tools/package_release.ps1")
    set_tests_properties(ayther.package.release_assets PROPERTIES
        TIMEOUT 30
        LABELS  "package;quality")

    add_test(
        NAME ayther.package.external_consumer_gate
        COMMAND "${AYTHER_PWSH_EXECUTABLE}" -NoProfile -File
            "${PROJECT_SOURCE_DIR}/tests/external_consumer_gate_test.ps1"
            -WorkDirectory
            "${PROJECT_BINARY_DIR}/tests/external_consumer_gate_contract"
            -Scanner
            "${PROJECT_SOURCE_DIR}/tools/check_external_consumer_paths.ps1"
            -Workflow
            "${PROJECT_SOURCE_DIR}/.github/workflows/release.yml"
            -RuntimeWorkflow
            "${PROJECT_SOURCE_DIR}/.github/workflows/runtime-integration.yml"
            -Verifier
            "${PROJECT_SOURCE_DIR}/tools/verify_staged_release_artifact.ps1"
            -Packager
            "${PROJECT_SOURCE_DIR}/tools/package_release.ps1")
    set_tests_properties(ayther.package.external_consumer_gate PROPERTIES
        TIMEOUT 30
        LABELS  "package;quality")
else()
    message(WARNING
        "pwsh was not found; PowerShell release contract tests are unavailable.")
endif()

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

# Link every public AytherRenderer method through the same target consumers use.
# No source/private include directory is added: this must remain a pure public
# header and Ayther::engine symbol-closure check.
add_executable(ayther_renderer_link_smoke
    "${PROJECT_SOURCE_DIR}/tests/contracts/renderer_link_smoke.cpp")
target_link_libraries(ayther_renderer_link_smoke PRIVATE Ayther::engine)
target_compile_features(ayther_renderer_link_smoke PRIVATE cxx_std_20)
ayther_instrument_target(ayther_renderer_link_smoke)
add_test(
    NAME ayther.package.renderer_link_smoke
    COMMAND ayther_renderer_link_smoke)
set_tests_properties(ayther.package.renderer_link_smoke PROPERTIES
    TIMEOUT 30
    LABELS  "engine;native;package")

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
list(APPEND _ayther_english_documented_headers engine/pack.hpp)
add_test(
    NAME ayther.quality.documentation_language
    COMMAND "${CMAKE_COMMAND}"
        "-DAYTHER_INCLUDE_DIR=${PROJECT_SOURCE_DIR}/include/ayther"
        "-DAYTHER_DOCUMENTED_HEADERS=${_ayther_english_documented_headers}"
        -P "${PROJECT_SOURCE_DIR}/tools/check_documentation_language.cmake")
set_tests_properties(ayther.quality.documentation_language PROPERTIES
    TIMEOUT 30
    LABELS  "engine;native;quality")

add_test(
    NAME ayther.quality.release_fixture_paths
    COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_FILE=${PROJECT_SOURCE_DIR}/tests/integration/elements_toml_test.cpp"
        "-DCMAKE_FILE=${PROJECT_SOURCE_DIR}/tests/integration/CMakeLists.txt"
        -P "${PROJECT_SOURCE_DIR}/tests/release_fixture_path_contract.cmake")
set_tests_properties(ayther.quality.release_fixture_paths PROPERTIES
    TIMEOUT 30
    LABELS  "audio_qa;quality;release")

add_test(
    NAME ayther.quality.spec002_docs
    COMMAND "${CMAKE_COMMAND}"
        "-DAYTHER_ROOT=${PROJECT_SOURCE_DIR}"
        -P "${PROJECT_SOURCE_DIR}/tests/engine_docs_contract.cmake")
set_tests_properties(ayther.quality.spec002_docs PROPERTIES
    TIMEOUT 30
    LABELS  "engine;native;quality")

add_subdirectory(unit)
add_subdirectory(integration)
add_subdirectory(audio_qa)
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

# The plane-set catalogue mutations (define, undefine, clear) rebuild the
# matcher's order; a live session with the test core drives them.
ayther_add_native_test(
    "${PROJECT_SOURCE_DIR}/tests/integration/plane_set_session_test.cpp"
    integration)
target_compile_definitions(ayther_plane_set_session_test PRIVATE
    AYTHER_TEST_CORE_PATH="$<TARGET_FILE:ayther_test_core>")
add_dependencies(ayther_plane_set_session_test ayther_test_core)

# Spec 002 (contracts.md C3): the render observation a live session publishes
# through Config::render_observer, driven with the test core.
ayther_add_native_test(
    "${PROJECT_SOURCE_DIR}/tests/integration/render_observation_session_test.cpp"
    integration)
target_compile_definitions(ayther_render_observation_session_test PRIVATE
    AYTHER_TEST_CORE_PATH="$<TARGET_FILE:ayther_test_core>")
add_dependencies(ayther_render_observation_session_test ayther_test_core)

# Spec 002 (RNF-2): the render observation neither changes the frame nor slows
# down its production; it measures the cost against the DA-2 budgets.
ayther_add_native_test(
    "${PROJECT_SOURCE_DIR}/tests/integration/render_observation_cost_test.cpp"
    integration)
target_compile_definitions(ayther_render_observation_cost_test PRIVATE
    AYTHER_TEST_CORE_PATH="$<TARGET_FILE:ayther_test_core>")
add_dependencies(ayther_render_observation_cost_test ayther_test_core)
set_tests_properties(ayther.integration.render_observation_cost_test
    PROPERTIES LABELS "engine;native;integration;performance")
# Coverage and sanitizer builds are unoptimised and instrumented: the test
# still measures, but its time budgets apply to optimised builds only.
if(AYTHER_ENABLE_COVERAGE OR AYTHER_ENABLE_ASAN OR AYTHER_ENABLE_UBSAN)
    target_compile_definitions(ayther_render_observation_cost_test PRIVATE
        AYTHER_INSTRUMENTED_BUILD=1)
endif()

# Spec 002 (contracts.md C4): export and restore of the visual state of a live
# session, section by section, driven with the test core.
ayther_add_native_test(
    "${PROJECT_SOURCE_DIR}/tests/integration/visual_state_session_test.cpp"
    integration)
target_compile_definitions(ayther_visual_state_session_test PRIVATE
    AYTHER_TEST_CORE_PATH="$<TARGET_FILE:ayther_test_core>")
add_dependencies(ayther_visual_state_session_test ayther_test_core)

# Spec 002 (contracts.md C4, BR-031): restoring the visual state at every frame
# of a synthetic take reproduces linear production for the next 30 frames.
ayther_add_native_test(
    "${PROJECT_SOURCE_DIR}/tests/integration/visual_state_roundtrip_all_frames.cpp"
    integration)
target_compile_definitions(ayther_visual_state_roundtrip_all_frames PRIVATE
    AYTHER_TEST_CORE_PATH="$<TARGET_FILE:ayther_test_core>")
add_dependencies(ayther_visual_state_roundtrip_all_frames ayther_test_core)
set_tests_properties(ayther.integration.visual_state_roundtrip_all_frames
    PROPERTIES TIMEOUT 600)

# Spec 002 phase 5 (plan §5.12): representation fixes with the test core and a
# trusted synthetic pack.
foreach(_ayther_fix_test IN ITEMS pose_anchor_test pose_membership_paths_test
        pose_partition_session_test
        framebuffer_judge_test
        scroll_tables_test
        pack_removal_test)
    ayther_add_native_test(
        "${PROJECT_SOURCE_DIR}/tests/integration/${_ayther_fix_test}.cpp"
        integration)
    target_compile_definitions("ayther_${_ayther_fix_test}" PRIVATE
        AYTHER_TEST_CORE_PATH="$<TARGET_FILE:ayther_test_core>")
    add_dependencies("ayther_${_ayther_fix_test}" ayther_test_core)
endforeach()
unset(_ayther_fix_test)

# Spec 002 (contracts.md C4, BR-032): silent production on SDL's dummy audio
# driver delivers no PCM and leaves the same HD audio state.
ayther_add_native_test(
    "${PROJECT_SOURCE_DIR}/tests/integration/silent_production_test.cpp"
    integration)
target_compile_definitions(ayther_silent_production_test PRIVATE
    AYTHER_TEST_CORE_PATH="$<TARGET_FILE:ayther_test_core>")
add_dependencies(ayther_silent_production_test ayther_test_core)
set_tests_properties(ayther.integration.silent_production_test PROPERTIES
    ENVIRONMENT "SDL_AUDIO_DRIVER=dummy")

# Spec 002 (P-9, DI-12): every audible frame reports its start on the device
# line; silent production reports none.
ayther_add_native_test(
    "${PROJECT_SOURCE_DIR}/tests/integration/frame_output_boundary_session_test.cpp"
    integration)
target_compile_definitions(ayther_frame_output_boundary_session_test PRIVATE
    AYTHER_TEST_CORE_PATH="$<TARGET_FILE:ayther_test_core>")
add_dependencies(ayther_frame_output_boundary_session_test ayther_test_core)
target_link_libraries(ayther_frame_output_boundary_session_test PRIVATE SDL3::SDL3)
set_tests_properties(ayther.integration.frame_output_boundary_session_test
    PROPERTIES ENVIRONMENT "SDL_AUDIO_DRIVER=dummy")

# Spec 002 (contracts.md C4, BR-033b): the pending HD audio exported in the
# middle of a take is complete and restores to the linear state.
ayther_add_native_test(
    "${PROJECT_SOURCE_DIR}/tests/integration/audio_pending_midtake_test.cpp"
    integration)
target_compile_definitions(ayther_audio_pending_midtake_test PRIVATE
    AYTHER_TEST_CORE_PATH="$<TARGET_FILE:ayther_test_core>")
add_dependencies(ayther_audio_pending_midtake_test ayther_test_core)
set_tests_properties(ayther.integration.audio_pending_midtake_test PROPERTIES
    ENVIRONMENT "SDL_AUDIO_DRIVER=dummy")

# Spec 002 (contracts.md C4, BR-033): pause with drain on the audio player,
# SDL's dummy driver capturing the device output.
ayther_add_native_test(
    "${PROJECT_SOURCE_DIR}/tests/integration/pause_after_drain_test.cpp"
    integration)
target_link_libraries(ayther_pause_after_drain_test PRIVATE SDL3::SDL3)
set_tests_properties(ayther.integration.pause_after_drain_test PROPERTIES
    ENVIRONMENT "SDL_AUDIO_DRIVER=dummy")

# Restoring cached audio events (set_audio_events) leaves the session as an
# analysis would; a live session with the test core drives it.
ayther_add_native_test(
    "${PROJECT_SOURCE_DIR}/tests/integration/audio_events_restore_test.cpp"
    integration)
target_compile_definitions(ayther_audio_events_restore_test PRIVATE
    AYTHER_TEST_CORE_PATH="$<TARGET_FILE:ayther_test_core>")
add_dependencies(ayther_audio_events_restore_test ayther_test_core)

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
    find_package(vk-bootstrap CONFIG REQUIRED)
    ayther_add_native_test(
        "${PROJECT_SOURCE_DIR}/tests/renderer/focus_layer_test.cpp" renderer)
    target_link_libraries(ayther_focus_layer_test PRIVATE
        SDL3::SDL3 GPUOpen::VulkanMemoryAllocator vk-bootstrap::vk-bootstrap)
    target_compile_definitions(ayther_focus_layer_test PRIVATE
        AYTHER_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    set_tests_properties(ayther.renderer.focus_layer_test PROPERTIES
        LABELS "engine;native;renderer;gpu")
    # Spec 002 (contracts.md C3): the renderer's draw report on a synthetic
    # scene, fed to the session's observation builder.
    ayther_add_native_test(
        "${PROJECT_SOURCE_DIR}/tests/renderer/render_draw_report_test.cpp"
        renderer)
    target_link_libraries(ayther_render_draw_report_test PRIVATE
        SDL3::SDL3 GPUOpen::VulkanMemoryAllocator vk-bootstrap::vk-bootstrap)
    target_compile_definitions(ayther_render_draw_report_test PRIVATE
        AYTHER_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    set_tests_properties(ayther.renderer.render_draw_report_test PROPERTIES
        LABELS "engine;native;renderer;gpu")
    # Spec 002, BR-089 (R6): a replacement hides its originals only with its
    # texture resident.
    ayther_add_native_test(
        "${PROJECT_SOURCE_DIR}/tests/renderer/texture_residency_gpu_test.cpp"
        renderer)
    target_link_libraries(ayther_texture_residency_gpu_test PRIVATE
        SDL3::SDL3 GPUOpen::VulkanMemoryAllocator vk-bootstrap::vk-bootstrap)
    target_compile_definitions(ayther_texture_residency_gpu_test PRIVATE
        AYTHER_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    set_tests_properties(ayther.renderer.texture_residency_gpu_test PROPERTIES
        LABELS "engine;native;renderer;gpu")
    # Spec 002, BR-090 (R6): flips by texture coordinates.
    ayther_add_native_test(
        "${PROJECT_SOURCE_DIR}/tests/renderer/sprite_flip_uv_test.cpp"
        renderer)
    target_link_libraries(ayther_sprite_flip_uv_test PRIVATE
        SDL3::SDL3 GPUOpen::VulkanMemoryAllocator vk-bootstrap::vk-bootstrap)
    target_compile_definitions(ayther_sprite_flip_uv_test PRIVATE
        AYTHER_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    set_tests_properties(ayther.renderer.sprite_flip_uv_test PROPERTIES
        LABELS "engine;native;renderer;gpu")
    # Spec 002, BR-091 (R6): the preparation prewarms the pack catalog.
    ayther_add_native_test(
        "${PROJECT_SOURCE_DIR}/tests/renderer/pack_prewarm_test.cpp"
        renderer)
    target_link_libraries(ayther_pack_prewarm_test PRIVATE
        SDL3::SDL3 GPUOpen::VulkanMemoryAllocator vk-bootstrap::vk-bootstrap)
    target_compile_definitions(ayther_pack_prewarm_test PRIVATE
        AYTHER_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
        AYTHER_TEST_CORE_PATH="$<TARGET_FILE:ayther_test_core>")
    add_dependencies(ayther_pack_prewarm_test ayther_test_core)
    set_tests_properties(ayther.renderer.pack_prewarm_test PROPERTIES
        LABELS "engine;native;renderer;gpu")
    # Spec 002, BR-092 (R6): a missing texture is decoded before the frame.
    ayther_add_native_test(
        "${PROJECT_SOURCE_DIR}/tests/renderer/texture_sync_decode_test.cpp"
        renderer)
    target_link_libraries(ayther_texture_sync_decode_test PRIVATE
        SDL3::SDL3 GPUOpen::VulkanMemoryAllocator vk-bootstrap::vk-bootstrap)
    target_compile_definitions(ayther_texture_sync_decode_test PRIVATE
        AYTHER_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    set_tests_properties(ayther.renderer.texture_sync_decode_test PROPERTIES
        LABELS "engine;native;renderer;gpu")
    # Spec 002, BR-093 (R6): an unreadable assigned asset is diagnosed.
    ayther_add_native_test(
        "${PROJECT_SOURCE_DIR}/tests/renderer/missing_asset_test.cpp"
        renderer)
    target_link_libraries(ayther_missing_asset_test PRIVATE
        SDL3::SDL3 GPUOpen::VulkanMemoryAllocator vk-bootstrap::vk-bootstrap)
    target_compile_definitions(ayther_missing_asset_test PRIVATE
        AYTHER_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    set_tests_properties(ayther.renderer.missing_asset_test PROPERTIES
        LABELS "engine;native;renderer;gpu")
    # Spec 002, R4 (BR-097, BR-098): the HD lanes in their pass (GPU oracle).
    ayther_add_native_test(
        "${PROJECT_SOURCE_DIR}/tests/renderer/hd_lane_order_test.cpp"
        renderer)
    target_link_libraries(ayther_hd_lane_order_test PRIVATE
        SDL3::SDL3 GPUOpen::VulkanMemoryAllocator vk-bootstrap::vk-bootstrap)
    target_compile_definitions(ayther_hd_lane_order_test PRIVATE
        AYTHER_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    set_tests_properties(ayther.renderer.hd_lane_order_test PROPERTIES
        LABELS "engine;native;renderer;gpu")
    # Spec 002, R3 (BR-100 to BR-102): the VDP sprite layer (GPU oracle).
    ayther_add_native_test(
        "${PROJECT_SOURCE_DIR}/tests/renderer/sprite_layer_order_test.cpp"
        renderer)
    target_link_libraries(ayther_sprite_layer_order_test PRIVATE
        SDL3::SDL3 GPUOpen::VulkanMemoryAllocator vk-bootstrap::vk-bootstrap)
    target_compile_definitions(ayther_sprite_layer_order_test PRIVATE
        AYTHER_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    set_tests_properties(ayther.renderer.sprite_layer_order_test PROPERTIES
        LABELS "engine;native;renderer;gpu")
    # Spec 002, R2 (BR-106, BR-107): pose replacements drawn by parts.
    ayther_add_native_test(
        "${PROJECT_SOURCE_DIR}/tests/renderer/pose_partition_gpu_test.cpp"
        renderer)
    target_link_libraries(ayther_pose_partition_gpu_test PRIVATE
        SDL3::SDL3 GPUOpen::VulkanMemoryAllocator vk-bootstrap::vk-bootstrap)
    target_compile_definitions(ayther_pose_partition_gpu_test PRIVATE
        AYTHER_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    set_tests_properties(ayther.renderer.pose_partition_gpu_test PROPERTIES
        LABELS "engine;native;renderer;gpu")
    # Spec 002, R8 (BR-111, BR-113): planes composed per scroll band.
    ayther_add_native_test(
        "${PROJECT_SOURCE_DIR}/tests/renderer/scroll_compose_gpu_test.cpp"
        renderer)
    target_link_libraries(ayther_scroll_compose_gpu_test PRIVATE
        SDL3::SDL3 GPUOpen::VulkanMemoryAllocator vk-bootstrap::vk-bootstrap)
    target_compile_definitions(ayther_scroll_compose_gpu_test PRIVATE
        AYTHER_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    set_tests_properties(ayther.renderer.scroll_compose_gpu_test PROPERTIES
        LABELS "engine;native;renderer;gpu")
    # Spec 002, R8 (BR-114): a raster frame shows the originals only.
    ayther_add_native_test(
        "${PROJECT_SOURCE_DIR}/tests/renderer/raster_frame_test.cpp" renderer)
    target_link_libraries(ayther_raster_frame_test PRIVATE
        SDL3::SDL3 GPUOpen::VulkanMemoryAllocator vk-bootstrap::vk-bootstrap)
    target_compile_definitions(ayther_raster_frame_test PRIVATE
        AYTHER_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    set_tests_properties(ayther.renderer.raster_frame_test PROPERTIES
        LABELS "engine;native;renderer;gpu")
    # Spec 002 (plan §5.13): the render probe and its test, which records a
    # take with the test core and runs the probe on it.
    add_subdirectory(
        "${PROJECT_SOURCE_DIR}/tools/render_probe"
        "${PROJECT_BINARY_DIR}/tools/render_probe")
    ayther_instrument_target(render_probe)
    ayther_add_native_test(
        "${PROJECT_SOURCE_DIR}/tests/renderer/render_probe_test.cpp" renderer)
    target_compile_definitions(ayther_render_probe_test PRIVATE
        AYTHER_TEST_CORE_PATH="$<TARGET_FILE:ayther_test_core>"
        RENDER_PROBE_PATH="$<TARGET_FILE:render_probe>")
    add_dependencies(ayther_render_probe_test ayther_test_core render_probe)
    set_tests_properties(ayther.renderer.render_probe_test PROPERTIES
        LABELS "engine;native;renderer;gpu" TIMEOUT 300)
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
            "${PROJECT_SOURCE_DIR}/include/ayther"
            "${PROJECT_SOURCE_DIR}/tests/support")
        target_link_libraries("${_ayther_gpu_test}" PRIVATE
            GPUOpen::VulkanMemoryAllocator
            vk-bootstrap::vk-bootstrap)
        ayther_instrument_target("${_ayther_gpu_test}")
    endforeach()
endif()

unset(_ayther_gpu_test)
unset(_ayther_native_tool_tests)
unset(_ayther_tool_test)
unset(_ayther_vpx_test)

# Audio pack prewarming must resolve archive IDs through the session as well.
target_compile_definitions(ayther_audio_asset_fallback_test PRIVATE
    AYTHER_TEST_CORE_PATH="$<TARGET_FILE:ayther_test_core>")
add_dependencies(ayther_audio_asset_fallback_test ayther_test_core)
