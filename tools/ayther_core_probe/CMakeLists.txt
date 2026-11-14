# ---------------------------------------------------------------------------
# ayther_core_probe — the installed core probe command (TP-013.5).
#
# It compiles src/core_probe.cpp directly instead of linking ayther_engine, so
# the installed tool has no dependency on SDL3, Vulkan or any other engine
# library and starts in the time a loader takes to map the core.
#
# Installed in bin/ with the probe vectors in share/ayther/probe-vectors/
# (TP-013.6): the fixture cores of tools/test_core and the expected answers.
# ---------------------------------------------------------------------------
add_executable(ayther_core_probe
    main.cpp
    "${PROJECT_SOURCE_DIR}/src/core_probe.cpp")
target_include_directories(ayther_core_probe PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/include/ayther")
target_compile_features(ayther_core_probe PRIVATE cxx_std_20)
target_link_libraries(ayther_core_probe PRIVATE
    $<BUILD_INTERFACE:ayther_strict_warnings>
    ${CMAKE_DL_LIBS})
set_target_properties(ayther_core_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# The expected answers of the shared vectors, next to the fixture cores.
set(AYTHER_PROBE_VECTORS_JSON
    "${CMAKE_CURRENT_SOURCE_DIR}/probe-vectors.json"
    CACHE INTERNAL "Expected answers of the core probe vectors")
