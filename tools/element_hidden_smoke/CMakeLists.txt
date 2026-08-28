# element_hidden_smoke (#273) — la visibilidad por elemento es una propiedad
# del inventario: ocultar un elemento cambia píxeles SOLO dentro de sus rects
# (criterio de región de R-4), el canal unificado set_hidden_elements produce
# byte a byte lo mismo que el canal específico, y limpiar restaura el frame
# exacto. Built only when -DAYTHER_BUILD_SPIKE=ON. ROM via AYTHER_PROBE_ROM.
add_executable(element_hidden_smoke main.cpp)
target_link_libraries(element_hidden_smoke PRIVATE ayther_engine ayther_core ${CMAKE_DL_LIBS})
if(WIN32)
    target_link_libraries(element_hidden_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(element_hidden_smoke PRIVATE cxx_std_20)
target_compile_definitions(element_hidden_smoke PRIVATE AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(element_hidden_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
