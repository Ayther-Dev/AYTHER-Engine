# layers_parallax_smoke (#276) — una capa insertada ENTRE plano B y plano A de
# un nivel real, moviendose a un ratio DERIVADO (B x 0.5), verificada por pixel
# sobre un frame de scroll grande: fase de la lamina = H*0.5 mod 64, z real
# (sobre B, bajo A), confinada a su banda. Requiere GPU (ventana SDL oculta).
# Built only when -DAYTHER_BUILD_SPIKE=ON. ROM via AYTHER_PROBE_ROM.
add_executable(layers_parallax_smoke main.cpp)
target_link_libraries(layers_parallax_smoke PRIVATE
    ayther_engine
    ayther_core
    SDL3::SDL3
    Vulkan::Vulkan
    ${CMAKE_DL_LIBS}
)
if(WIN32)
    target_link_libraries(layers_parallax_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(layers_parallax_smoke PRIVATE cxx_std_20)
target_compile_definitions(layers_parallax_smoke PRIVATE AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(layers_parallax_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
