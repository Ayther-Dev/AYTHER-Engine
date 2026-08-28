# ---------------------------------------------------------------------------
# sh_probe — E3 (Estados de una Pose): ¿alguna toma ejercita el modo Shadow/
# Highlight del VDP (reg 0x0C bit 3)? Base de validación antes de tocar el
# render. Lee tests/test_config.toml. Built only when -DAYTHER_BUILD_SPIKE=ON.
# ---------------------------------------------------------------------------

add_executable(sh_probe main.cpp)

target_link_libraries(sh_probe PRIVATE
    ayther_engine
    ayther_core
    ${CMAKE_DL_LIBS}
)

if(WIN32)
    target_link_libraries(sh_probe PRIVATE ws2_32 userenv bcrypt ntdll)
endif()

target_compile_features(sh_probe PRIVATE cxx_std_20)
target_compile_definitions(sh_probe PRIVATE
    AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

set_target_properties(sh_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
