# ---------------------------------------------------------------------------
# ABI and handle-lifecycle tests for the ayther_core boundary.
#
# These link only against Ayther::core, so they build and run under the
# headless presets with no SDL3, Vulkan, or GPU present.
# ---------------------------------------------------------------------------

add_executable(ayther_core_ffi_tests ayther_core_ffi_test.cpp)

target_link_libraries(ayther_core_ffi_tests PRIVATE
    Ayther::core
    ayther_test_support)
ayther_instrument_target(ayther_core_ffi_tests)

target_compile_definitions(ayther_core_ffi_tests PRIVATE
    "AYTHER_CMAKE_PROJECT_VERSION=\"${PROJECT_VERSION}\"")

if(MSVC)
    target_compile_options(ayther_core_ffi_tests PRIVATE /W4)
else()
    target_compile_options(ayther_core_ffi_tests PRIVATE -Wall -Wextra)
endif()

add_test(NAME ayther.core.ffi COMMAND ayther_core_ffi_tests)

set_tests_properties(ayther.core.ffi PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    TIMEOUT           120
    LABELS            "core;abi;headless;ffi")

# ---------------------------------------------------------------------------
# Trust policy at the C boundary: rotation windows, revocation, and per-game
# scope. Separate from the ABI/handle suite because it writes registries and
# bakes packs on disk, and a reader looking for the trust contract should find
# it in one file.
# ---------------------------------------------------------------------------
add_executable(ayther_pack_trust_ffi_tests pack_trust_ffi_test.cpp)

target_link_libraries(ayther_pack_trust_ffi_tests PRIVATE
    Ayther::core
    ayther_test_support)
ayther_instrument_target(ayther_pack_trust_ffi_tests)

if(MSVC)
    target_compile_options(ayther_pack_trust_ffi_tests PRIVATE /W4)
else()
    target_compile_options(ayther_pack_trust_ffi_tests PRIVATE -Wall -Wextra)
endif()

add_test(NAME ayther.core.pack_trust_ffi COMMAND ayther_pack_trust_ffi_tests)

set_tests_properties(ayther.core.pack_trust_ffi PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    TIMEOUT           120
    LABELS            "core;security;headless;ffi")
