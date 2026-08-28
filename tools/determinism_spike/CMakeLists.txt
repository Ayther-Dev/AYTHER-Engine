# ---------------------------------------------------------------------------
# determinism_spike — headless harness validating risk #1 (savestate
# re-simulation determinism). No SDL / Vulkan: compiles RetroRunner +
# core_loader (header) only. See docs/architecture/ayther-engine.md §6.1.
#
# Built only when -DAYTHER_BUILD_SPIKE=ON.
# ---------------------------------------------------------------------------

add_executable(determinism_spike
    spike.cpp
    ${CMAKE_SOURCE_DIR}/src/retro_runner.cpp   # motor tech (post-R1 migration)
)

target_include_directories(determinism_spike PRIVATE
    ${CMAKE_SOURCE_DIR}/include/ayther
)

target_compile_features(determinism_spike PRIVATE cxx_std_20)

# #230 EM-7.4: `RetroRunner` aplica el parche IPS/BPS del usuario, que vive en
# el core. El spike no lo usa —nunca fija un patch_path— pero el simbolo tiene
# que resolver igual: un enlace incompleto no es un test que se saltea, es un
# binario que no existe.
target_link_libraries(determinism_spike PRIVATE ayther_core)

set_target_properties(determinism_spike PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
