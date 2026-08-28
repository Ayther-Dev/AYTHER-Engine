# ---------------------------------------------------------------------------
# ABI and handle-lifecycle tests for the ayther_core boundary.
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
    LABELS            "core;abi;headless;ffi")
