# paint_repalette_smoke (2026-07-31) — el re-paletado de un hash de tile de
# plano es EXACTO: contrasta ayther_plane_tile_hash_repalette contra los hashes
# que el motor ya calculó, sobre pares reales (mismo patrón, distinta paleta) de
# una toma. Es lo que habilita clonar un Carácter a otro color sin exigir que la
# variante esté en pantalla. Built only when -DAYTHER_BUILD_SPIKE=ON.
# Args: <rec.arp> [f0] [f1] · ROM via AYTHER_PROBE_ROM.
add_executable(paint_repalette_smoke main.cpp)
target_link_libraries(paint_repalette_smoke PRIVATE ayther_engine ayther_core ${CMAKE_DL_LIBS})
if(WIN32)
    target_link_libraries(paint_repalette_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(paint_repalette_smoke PRIVATE cxx_std_20)
target_compile_definitions(paint_repalette_smoke PRIVATE AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(paint_repalette_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
