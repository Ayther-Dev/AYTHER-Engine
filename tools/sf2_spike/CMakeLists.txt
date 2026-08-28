# ---------------------------------------------------------------------------
# sf2_spike — recupera la PARTITURA que el juego ejecuta desde las escrituras al
# bus de los chips (FM + PSG), para decidir si vale re-sintetizarla con un
# SoundFont. Lee tests/test_config.toml (o argv).
# Built only when -DAYTHER_BUILD_SPIKE=ON.
# ---------------------------------------------------------------------------

add_executable(sf2_spike main.cpp)

target_link_libraries(sf2_spike PRIVATE
    ayther_engine
    ayther_core
    ${CMAKE_DL_LIBS}
)

if(WIN32)
    target_link_libraries(sf2_spike PRIVATE ws2_32 userenv bcrypt ntdll)
endif()

target_compile_features(sf2_spike PRIVATE cxx_std_20)
target_compile_definitions(sf2_spike PRIVATE
    AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

set_target_properties(sf2_spike PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
