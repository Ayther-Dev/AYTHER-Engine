# ---------------------------------------------------------------------------
# audio_output_smoke — el oráculo de que el Lab SUENA, con una ROM SINTÉTICA.
#
# Entra al ctest NORMAL, y ése es todo el punto: los otros oráculos de audio
# necesitan una ROM comercial que no se commitea, así que viven fuera de la
# corrida diaria y se ejecutan cuando alguien se acuerda. Éste se fabrica su
# propio Mega Drive (68k escrito en el .cpp), así que no hay licencia que
# discutir ni corpus que preparar.
#
# Necesita el core del FORK (third_party/cores) porque mide el camino de audio
# entero; si no está, se saltea con un [skip] visible.
# ---------------------------------------------------------------------------
add_executable(audio_output_smoke main.cpp)
target_link_libraries(audio_output_smoke PRIVATE
    ayther_engine
    ayther_core
    SDL3::SDL3
    ${CMAKE_DL_LIBS}
)
if(WIN32)
    target_link_libraries(audio_output_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(audio_output_smoke PRIVATE cxx_std_20)
target_compile_definitions(audio_output_smoke PRIVATE AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(audio_output_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_test(NAME audio_output COMMAND audio_output_smoke)
# Sin el core del fork el oráculo no puede medir NADA, y `third_party/cores/`
# está en .gitignore (9,5 MB por DLL) — o sea que en CI no está. Salir con 0
# ahí lo dejaba contado como «Passed»: verde vacío, que es justo el defecto que
# esta issue combate. Con el 77, ctest lo reporta como SALTEADO y el resumen
# dice la verdad. Medido: en CI el test tardaba 0,02 s contra 2,7 s acá.
set_tests_properties(audio_output PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT          120
    LABELS           "engine;native;integration;audio")
