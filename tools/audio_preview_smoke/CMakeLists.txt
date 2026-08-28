# ---------------------------------------------------------------------------
# audio_preview_smoke — prueba de integración de AytherSession::preview_audio.
# Linkea el motor completo (ayther_engine) + el core Rust. Lee tests/test_config.toml
# (rutas locales de core/ROM) → AYTHER_SOURCE_DIR apunta a la raíz del repo.
# Built only when -DAYTHER_BUILD_SPIKE=ON.
# ---------------------------------------------------------------------------

add_executable(audio_preview_smoke main.cpp)

target_link_libraries(audio_preview_smoke PRIVATE
    ayther_engine     # AytherSession + RetroRunner + AudioPlayer (SDL3 PUBLIC)
    ayther_core       # símbolos FFI del core Rust
    ${CMAKE_DL_LIBS}
)

if(WIN32)
    target_link_libraries(audio_preview_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()

target_compile_features(audio_preview_smoke PRIVATE cxx_std_20)
target_compile_definitions(audio_preview_smoke PRIVATE
    AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

set_target_properties(audio_preview_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
