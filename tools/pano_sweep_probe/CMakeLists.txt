# ---------------------------------------------------------------------------
# pano_sweep_probe — barrido de Panorámica headless (#349): mismo camino que
# «Crear Panorámica» (captura del stitcher + export del plano) sin UI, para
# validar la cámara de CONTENIDO contra el oráculo del artista.
#
# Built only when -DAYTHER_BUILD_SPIKE=ON. ROM/toma por argumentos (BYOR).
# ---------------------------------------------------------------------------

add_executable(pano_sweep_probe main.cpp)

# El engine declara stbi_write_png pero la TU de implementación vive en el
# Lab — un tool headless tiene que traer la suya (main.cpp la define).
find_package(Stb REQUIRED)
target_include_directories(pano_sweep_probe PRIVATE ${Stb_INCLUDE_DIR})

target_link_libraries(pano_sweep_probe PRIVATE
    ayther_engine
    ayther_core
    ${CMAKE_DL_LIBS}
)

if(WIN32)
    target_link_libraries(pano_sweep_probe PRIVATE ws2_32 userenv bcrypt ntdll)
endif()

target_compile_features(pano_sweep_probe PRIVATE cxx_std_20)

set_target_properties(pano_sweep_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
