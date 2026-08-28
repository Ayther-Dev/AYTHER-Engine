# pano_band_smoke (#421) — la Panoramica emite un quad POR BANDA, verificado
# sobre Sonic 3 & Knuckles, que es donde el caso EXISTE (medido: 37 bandas en el
# plano B; Golden Axe, el ejemplo original de la issue, tiene 0 en 40.854
# frames). Graba el boot en vivo: no necesita una toma del proyecto.
# Sin GPU — es el camino sesion, no el render.
add_executable(pano_band_smoke main.cpp)
target_link_libraries(pano_band_smoke PRIVATE
    ayther_engine
    ayther_core
    ${CMAKE_DL_LIBS}
)
if(WIN32)
    target_link_libraries(pano_band_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(pano_band_smoke PRIVATE cxx_std_20)
target_compile_definitions(pano_band_smoke PRIVATE AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(pano_band_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
