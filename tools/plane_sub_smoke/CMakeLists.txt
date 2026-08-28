# ---------------------------------------------------------------------------
# plane_sub_smoke — valida el resolver scroll-aware de la Fase 2c (posiciones de
# overlay HD de tiles de plano) contra el oráculo de la Fase 2b. Lee
# tests/test_config.toml. Built only when -DAYTHER_BUILD_SPIKE=ON.
# ---------------------------------------------------------------------------

add_executable(plane_sub_smoke main.cpp)

target_link_libraries(plane_sub_smoke PRIVATE
    ayther_engine
    ayther_core
    ${CMAKE_DL_LIBS}
)

if(WIN32)
    target_link_libraries(plane_sub_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()

target_compile_features(plane_sub_smoke PRIVATE cxx_std_20)
target_compile_definitions(plane_sub_smoke PRIVATE
    AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

set_target_properties(plane_sub_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
