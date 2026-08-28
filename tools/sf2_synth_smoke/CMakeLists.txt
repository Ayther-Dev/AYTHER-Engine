# ---------------------------------------------------------------------------
# sf2_synth_smoke — el sintetizador SoundFont cruza el FFI y suena (#261).
# Ver main.cpp. Linkea el core directo (no el engine): lo que se prueba es la
# frontera Rust↔C++, y meter el motor en el medio agregaría formas de fallar
# que no tienen que ver con eso.
# ---------------------------------------------------------------------------

add_executable(sf2_synth_smoke main.cpp)

target_link_libraries(sf2_synth_smoke PRIVATE ayther_core ${CMAKE_DL_LIBS})
target_include_directories(sf2_synth_smoke PRIVATE ${CMAKE_SOURCE_DIR}/include/ayther)

if(WIN32)
    target_link_libraries(sf2_synth_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()

target_compile_features(sf2_synth_smoke PRIVATE cxx_std_20)
set_target_properties(sf2_synth_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Sin argumentos verifica el camino NULO —el que el motor encuentra cuando no
# hay SoundFont asignado— y sale verde. El ctest tiene que correr en una máquina
# sin SoundFonts.
add_test(NAME sf2_synth COMMAND sf2_synth_smoke)
set_tests_properties(sf2_synth PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    TIMEOUT           120
    LABELS            "core;ffi;native;headless")
