# clean_theme_probe (#563 / #507) — graba un tema LIMPIO por id, repetible.
#
# Es el flujo que el botón «Grabar tema limpio» de Mezclar necesita, sin UI: con
# una lista de ids se graban los cuarenta temas de un juego en una corrida, que
# es el volumen que la issue dice esperar antes de automatizar.
#
# Sin GPU: el mixdown re-simula y no necesita device de audio.
add_executable(clean_theme_probe main.cpp)
target_link_libraries(clean_theme_probe PRIVATE ayther_engine ayther_core ${CMAKE_DL_LIBS})
if(WIN32)
    target_link_libraries(clean_theme_probe PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(clean_theme_probe PRIVATE cxx_std_20)
set_target_properties(clean_theme_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
