# overlay_fx_smoke (#352/#353/#354) — los tres huecos del Acetato por pixel:
# opacidad/blend contra la formula de composicion, tileado 2D con repeticion
# exacta y tope de quads, deriva propia determinista (fase pura del frame,
# seek por dos caminos, pausa). Requiere GPU (ventana SDL oculta).
# Built only when -DAYTHER_BUILD_SPIKE=ON. ROM via AYTHER_PROBE_ROM.
add_executable(overlay_fx_smoke main.cpp)
target_link_libraries(overlay_fx_smoke PRIVATE
    ayther_engine
    ayther_core
    SDL3::SDL3
    Vulkan::Vulkan
    ${CMAKE_DL_LIBS}
)
if(WIN32)
    target_link_libraries(overlay_fx_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(overlay_fx_smoke PRIVATE cxx_std_20)
target_compile_definitions(overlay_fx_smoke PRIVATE AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(overlay_fx_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
