# ---------------------------------------------------------------------------
# panorama_smoke — valida el ANCLAJE POR CONTENIDO de la Panorámica (CU003).
# Coincide con un unwrap independiente y SOBREVIVE A UN SEEK, que es lo que
# la cámara acumulada de EM-1 no puede. Lee tests/test_config.toml (o argv).
# Built only when -DAYTHER_BUILD_SPIKE=ON.
# ---------------------------------------------------------------------------

add_executable(panorama_smoke main.cpp)

target_link_libraries(panorama_smoke PRIVATE
    ayther_engine
    ayther_core
    ${CMAKE_DL_LIBS}
)

if(WIN32)
    target_link_libraries(panorama_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()

target_compile_features(panorama_smoke PRIVATE cxx_std_20)
target_compile_definitions(panorama_smoke PRIVATE
    AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

set_target_properties(panorama_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
