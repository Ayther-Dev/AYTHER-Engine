# widescreen_gate_smoke (#231 EM-8.2) — el ancho lo decide el PACK por frame, con
# la RAM viva. Los tests del core fijan el gate como funcion pura; esto prueba el
# CABLEADO: que el ancho llegue al frame sin desfase y que desarmar el gate
# devuelva intacto el pedido manual. Sin GPU.
add_executable(widescreen_gate_smoke main.cpp)
target_link_libraries(widescreen_gate_smoke PRIVATE ayther_engine ayther_core ${CMAKE_DL_LIBS})
if(WIN32)
    target_link_libraries(widescreen_gate_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(widescreen_gate_smoke PRIVATE cxx_std_20)
target_compile_definitions(widescreen_gate_smoke PRIVATE AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(widescreen_gate_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
