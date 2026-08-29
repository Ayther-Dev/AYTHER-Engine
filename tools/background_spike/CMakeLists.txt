# ---------------------------------------------------------------------------
# background_spike — headless harness validating the Fondos motor: stitch a
# scrolling plane into a full level strip + unwrap the wrapped VDP Hscroll, vs a
# real ROM. No SDL / Vulkan (like determinism_spike); links ayther_core for
# BackgroundStitcher + ScrollUnwrapper. See docs/COMPONENT_MODEL.md#backgrounds.
#
# Built only when -DAYTHER_BUILD_SPIKE=ON.
# ---------------------------------------------------------------------------

add_executable(background_spike
    spike.cpp
    ${CMAKE_SOURCE_DIR}/src/retro_runner.cpp
)

target_include_directories(background_spike PRIVATE
    ${CMAKE_SOURCE_DIR}/include/ayther
)

target_link_libraries(background_spike PRIVATE
    ayther_core
    ${CMAKE_DL_LIBS}
)

if(WIN32)
    target_link_libraries(background_spike PRIVATE ws2_32 userenv bcrypt ntdll advapi32)
endif()

target_compile_features(background_spike PRIVATE cxx_std_20)

set_target_properties(background_spike PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
