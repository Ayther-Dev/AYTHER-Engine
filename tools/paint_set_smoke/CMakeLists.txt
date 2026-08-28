# ---------------------------------------------------------------------------
# paint_set_smoke — valida el matcher de plane SETS de Pintar Fase C (quad
# multi-tile por aparición + supresión de miembros + lane por prioridad) contra
# el oráculo de posición de plane_sub_smoke. Lee tests/test_config.toml.
# Built only when -DAYTHER_BUILD_SPIKE=ON.
# ---------------------------------------------------------------------------

add_executable(paint_set_smoke main.cpp)

target_link_libraries(paint_set_smoke PRIVATE
    ayther_engine
    ayther_core
    ${CMAKE_DL_LIBS}
)

if(WIN32)
    target_link_libraries(paint_set_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()

target_compile_features(paint_set_smoke PRIVATE cxx_std_20)
target_compile_definitions(paint_set_smoke PRIVATE
    AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

set_target_properties(paint_set_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
