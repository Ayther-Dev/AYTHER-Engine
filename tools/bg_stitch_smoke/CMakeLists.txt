# ---------------------------------------------------------------------------
# bg_stitch_smoke — valida el BackgroundStitcher (Fondos, Caso B/C): reconstruye
# la tira completa de un plano que scrollea y verifica el des-enrollado del
# scroll. Lee tests/test_config.toml. Built only when -DAYTHER_BUILD_SPIKE=ON.
# ---------------------------------------------------------------------------

add_executable(bg_stitch_smoke main.cpp)

target_link_libraries(bg_stitch_smoke PRIVATE
    ayther_engine
    ayther_core
    ${CMAKE_DL_LIBS}
)

if(WIN32)
    target_link_libraries(bg_stitch_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()

target_compile_features(bg_stitch_smoke PRIVATE cxx_std_20)
target_compile_definitions(bg_stitch_smoke PRIVATE
    AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

set_target_properties(bg_stitch_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
