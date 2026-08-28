# focus_layer_smoke (#372) — el atenuado sigue a la capa que se autora: la capa
# enfocada queda byte-exacta y las otras cumplen por pixel la formula del
# compositor (tinte 0.25 Y opacidad 75%, con el fondo leido ocultando el
# elemento). Requiere GPU (ventana SDL oculta).
# Built only when -DAYTHER_BUILD_SPIKE=ON. ROM via AYTHER_PROBE_ROM.
add_executable(focus_layer_smoke main.cpp)
target_link_libraries(focus_layer_smoke PRIVATE
    ayther_engine
    ayther_core
    SDL3::SDL3
    Vulkan::Vulkan
    ${CMAKE_DL_LIBS}
)
if(WIN32)
    target_link_libraries(focus_layer_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(focus_layer_smoke PRIVATE cxx_std_20)
target_compile_definitions(focus_layer_smoke PRIVATE AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(focus_layer_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
