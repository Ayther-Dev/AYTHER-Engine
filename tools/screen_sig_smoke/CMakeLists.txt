# ---------------------------------------------------------------------------
# screen_sig_smoke — valida la firma de PANTALLA (Cuadro · CU001).
# Estabilidad · discriminación · composición por capas · tolerancia a la fase
# sub-celda del scroll. Lee tests/test_config.toml (o argv).
# Built only when -DAYTHER_BUILD_SPIKE=ON.
# ---------------------------------------------------------------------------

add_executable(screen_sig_smoke main.cpp)

target_link_libraries(screen_sig_smoke PRIVATE
    ayther_engine
    ayther_core
    ${CMAKE_DL_LIBS}
)

if(WIN32)
    target_link_libraries(screen_sig_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()

target_compile_features(screen_sig_smoke PRIVATE cxx_std_20)
target_compile_definitions(screen_sig_smoke PRIVATE
    AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

set_target_properties(screen_sig_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
