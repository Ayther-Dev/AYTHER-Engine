# overlay_blend_smoke (#485/#486) — los cuatro modos de mezcla del Acetato
# (alpha, aditivo, multiplicar, pantalla) EJECUTADOS en la GPU y comparados
# por píxel contra sus fórmulas. Sin ROM ni grabación: dos capas Custom (base
# opaca conocida + lámina de test con bandas de alpha) y un FrameView armado a
# mano. Requiere GPU (ventana SDL oculta) — detrás de AYTHER_BUILD_SPIKE, como
# el resto de los oráculos de readback (molde: overlay_vscroll_smoke).
add_executable(overlay_blend_smoke main.cpp)
target_link_libraries(overlay_blend_smoke PRIVATE
    ayther_engine
    ayther_core
    SDL3::SDL3
    Vulkan::Vulkan
    ${CMAKE_DL_LIBS}
)
if(WIN32)
    target_link_libraries(overlay_blend_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(overlay_blend_smoke PRIVATE cxx_std_20)
target_compile_definitions(overlay_blend_smoke PRIVATE AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(overlay_blend_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Entra al ctest: solo necesita GPU, mismo criterio que sprite_mask/acetato_vs.
add_test(NAME acetato_blend COMMAND overlay_blend_smoke)
set_tests_properties(acetato_blend PROPERTIES
    TIMEOUT 120
    LABELS  "engine;native;gpu")
