# ---------------------------------------------------------------------------
# plane_hide_smoke — prueba de integración de la Fase 2b (ocultar tile de plano,
# id 0x105). Linkea el motor completo + el core Rust. Lee tests/test_config.toml.
# Built only when -DAYTHER_BUILD_SPIKE=ON.
# ---------------------------------------------------------------------------

add_executable(plane_hide_smoke main.cpp)

target_link_libraries(plane_hide_smoke PRIVATE
    ayther_engine     # AytherSession + RetroRunner
    ayther_core       # símbolos FFI del core Rust
    ${CMAKE_DL_LIBS}
)

if(WIN32)
    target_link_libraries(plane_hide_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()

target_compile_features(plane_hide_smoke PRIVATE cxx_std_20)
target_compile_definitions(plane_hide_smoke PRIVATE
    AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

set_target_properties(plane_hide_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
