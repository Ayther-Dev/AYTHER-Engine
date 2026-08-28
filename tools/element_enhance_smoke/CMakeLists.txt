# element_enhance_smoke (#493) — la mejora por software es una propiedad del
# inventario por (capa, hash): marcar publica fx_enhance=1 sólo en el elemento,
# un elemento reclamado por HD no se mejora (el asset ganó), ruteo por capa y
# limpiar deja todo apagado. Sin GPU. Built only when -DAYTHER_BUILD_SPIKE=ON. ROM via AYTHER_PROBE_ROM.
add_executable(element_enhance_smoke main.cpp)
target_link_libraries(element_enhance_smoke PRIVATE ayther_engine ayther_core ${CMAKE_DL_LIBS})
if(WIN32)
    target_link_libraries(element_enhance_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(element_enhance_smoke PRIVATE cxx_std_20)
target_compile_definitions(element_enhance_smoke PRIVATE AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(element_enhance_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
