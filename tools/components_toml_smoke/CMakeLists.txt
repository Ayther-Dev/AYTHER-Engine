# ---------------------------------------------------------------------------
# components_toml_smoke — valida el round-trip TOML de Componentes (animations +
# audio_events, bake y parse engine-side), sin emulador.
# Built only when -DAYTHER_BUILD_SPIKE=ON.
# ---------------------------------------------------------------------------

add_executable(components_toml_smoke main.cpp)

target_link_libraries(components_toml_smoke PRIVATE
    ayther_engine
    ayther_core
    ${CMAKE_DL_LIBS}
)

if(WIN32)
    target_link_libraries(components_toml_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()

target_compile_features(components_toml_smoke PRIVATE cxx_std_20)
target_compile_definitions(components_toml_smoke PRIVATE
    AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

set_target_properties(components_toml_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
