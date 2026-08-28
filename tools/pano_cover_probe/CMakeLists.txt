# pano_cover_probe (#565) — mide la COBERTURA de una Panorámica sobre un juego
# real. El oráculo `panorama_cover` fija la REGLA con tres hashes inventados;
# esto mide el EFECTO del arreglo, que es lo que un test unitario no puede ver:
# si la tira es ambigua en muchas posiciones la cobertura baja, y un arreglo que
# evita mostrar el tramo equivocado apagando la función no arregló nada.
#
# Sin GPU: sólo mira los números del FrameView. ROM y core por argumento.
add_executable(pano_cover_probe main.cpp)
target_link_libraries(pano_cover_probe PRIVATE ayther_engine ayther_core ${CMAKE_DL_LIBS})
if(WIN32)
    target_link_libraries(pano_cover_probe PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(pano_cover_probe PRIVATE cxx_std_20)
set_target_properties(pano_cover_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
