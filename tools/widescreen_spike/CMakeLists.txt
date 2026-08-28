# ---------------------------------------------------------------------------
# widescreen_spike — mide cuántas columnas laterales de la nametable contienen
# arte de nivel válido (vs basura) durante gameplay con scroll: el de-risk de
# EM-8 (#231) ANTES de tocar el render. Lee tests/test_config.toml (o argv).
# Built only when -DAYTHER_BUILD_SPIKE=ON.
# ---------------------------------------------------------------------------

add_executable(widescreen_spike main.cpp)

target_link_libraries(widescreen_spike PRIVATE
    ayther_engine
    ayther_core
    ${CMAKE_DL_LIBS}
)

if(WIN32)
    target_link_libraries(widescreen_spike PRIVATE ws2_32 userenv bcrypt ntdll)
endif()

target_compile_features(widescreen_spike PRIVATE cxx_std_20)
target_compile_definitions(widescreen_spike PRIVATE
    AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

set_target_properties(widescreen_spike PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
