# ---------------------------------------------------------------------------
# vpx_smoke — verifica la integración de libvpx (#263). Ver main.cpp.
#
# NO linkea el motor a propósito: lo que se prueba es que el .lib de libvpx
# construido con MSBuild enlaza con clang-cl y que el decoder de VP9 sobrevivió
# al recorte del configure. Meter el motor en el medio sólo agregaría formas de
# fallar que no tienen que ver con lo que se está verificando.
#
# Sólo existe si libvpx está construido (ver el CMakeLists raíz).
# ---------------------------------------------------------------------------

add_executable(vpx_smoke main.cpp)

target_link_libraries(vpx_smoke PRIVATE ayther::vpx)

target_compile_features(vpx_smoke PRIVATE cxx_std_20)

set_target_properties(vpx_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

add_test(NAME vpx_integration COMMAND vpx_smoke)
set_tests_properties(vpx_integration PROPERTIES
    TIMEOUT 120
    LABELS  "native;integration;vpx")
