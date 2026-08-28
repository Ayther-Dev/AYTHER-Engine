# ---------------------------------------------------------------------------
# mode3_spike — headless harness validating risk #2 (VDP scroll reads) and
# Mode 3 (RAM anchoring) against a real ROM. No SDL / Vulkan: compiles
# RetroRunner (like determinism_spike) and links the Rust core (ayther_core)
# for ram_anchor::assign_sprites + the SAT parser. See
# docs/design/estado-motor-componentes.md §3.4/§3.5.
#
# Built only when -DAYTHER_BUILD_SPIKE=ON.
# ---------------------------------------------------------------------------

add_executable(mode3_spike
    spike.cpp
    ${CMAKE_SOURCE_DIR}/src/retro_runner.cpp   # motor tech, header-only deps
)

target_include_directories(mode3_spike PRIVATE
    ${CMAKE_SOURCE_DIR}/include/ayther
)

# Rust static lib: ayther_mode3_assign_sprites + ayther_sprite_hasher_* .
target_link_libraries(mode3_spike PRIVATE
    ayther_core
    ${CMAKE_DL_LIBS}
)

if(WIN32)
    # Windows system libs required by the Rust standard library.
    target_link_libraries(mode3_spike PRIVATE ws2_32 userenv bcrypt ntdll advapi32)
endif()

target_compile_features(mode3_spike PRIVATE cxx_std_20)

set_target_properties(mode3_spike PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
