# element_effect_smoke (#275) — los efectos son una propiedad del elemento:
# tinte Q2.6 (formula del shader verificada por pixel), opacidad y silueta,
# aplicados por el compose como uniform por quad y confinados a las celdas del
# elemento. Requiere GPU (ventana SDL oculta).
# Built only when -DAYTHER_BUILD_SPIKE=ON. ROM via AYTHER_PROBE_ROM.
add_executable(element_effect_smoke main.cpp)
target_link_libraries(element_effect_smoke PRIVATE
    ayther_engine
    ayther_core
    SDL3::SDL3
    Vulkan::Vulkan
    ${CMAKE_DL_LIBS}
)
if(WIN32)
    target_link_libraries(element_effect_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(element_effect_smoke PRIVATE cxx_std_20)
target_compile_definitions(element_effect_smoke PRIVATE AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(element_effect_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
