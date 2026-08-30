# ---------------------------------------------------------------------------
# ayther_test_core — the emulator core the ABI oracles were missing.
#
# Two shared libraries from one source. The only difference is whether
# `ayther_get_interface` is exported, which is exactly the axis
# `abi_negociacion` measures: a core WITH the ABI must negotiate and declare its
# capabilities, and a core WITHOUT it must still load, because the negotiation
# is additive and may not break anyone.
#
# These are MODULE libraries: they are dlopen'd by the engine at runtime, never
# linked against. They are also NOT installed -- a test fixture has no business
# in a release artifact.
# ---------------------------------------------------------------------------

function(ayther_add_test_core _target)
    add_library("${_target}" MODULE ayther_test_core.cpp)

    target_include_directories("${_target}" PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/include/ayther")
    target_compile_features("${_target}" PRIVATE cxx_std_20)

    # AYTHER_API is dllexport only for the side that IS the core. Without this
    # the descriptor entry point compiles but never reaches the export table,
    # and the frontend sees a core that does not speak the ABI.
    target_compile_definitions("${_target}" PRIVATE AYTHER_CORE_EXPORTS)

    # No "lib" prefix and a platform-native suffix, so the loader finds it by
    # the same name on both platforms.
    set_target_properties("${_target}" PROPERTIES
        PREFIX ""
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

    # A core is dlopen'd, so every symbol it exports has to be visible whatever
    # the project default is.
    if(NOT MSVC)
        target_compile_options("${_target}" PRIVATE -fvisibility=default)
    endif()
endfunction()

ayther_add_test_core(ayther_test_core)

ayther_add_test_core(ayther_test_core_stock)
target_compile_definitions(ayther_test_core_stock PRIVATE AYTHER_TEST_CORE_NO_ABI)

# The oracles locate the cores by path. Publishing them as properties keeps the
# knowledge in one place instead of repeating the layout in every test.
set(AYTHER_TEST_CORE_PATH "$<TARGET_FILE:ayther_test_core>"
    CACHE INTERNAL "Test core implementing the AYTHER ABI")
set(AYTHER_TEST_CORE_STOCK_PATH "$<TARGET_FILE:ayther_test_core_stock>"
    CACHE INTERNAL "Test core without the AYTHER ABI")
