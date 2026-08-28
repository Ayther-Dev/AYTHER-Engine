# scene_compose_smoke (#274) — el renderer compone la escena SIN el blit
# (pipeline indexado alimentado por fv.scene) y el resultado se compara byte a
# byte contra el framebuffer del emulador del mismo frame — el presupuesto de
# R-1. Requiere GPU (ventana SDL oculta).
# Built only when -DAYTHER_BUILD_SPIKE=ON. ROM via AYTHER_PROBE_ROM.
add_executable(scene_compose_smoke main.cpp)
target_link_libraries(scene_compose_smoke PRIVATE
    ayther_engine
    ayther_core
    SDL3::SDL3
    Vulkan::Vulkan
    ${CMAKE_DL_LIBS}
)
if(WIN32)
    target_link_libraries(scene_compose_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(scene_compose_smoke PRIVATE cxx_std_20)
target_compile_definitions(scene_compose_smoke PRIVATE AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(scene_compose_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
