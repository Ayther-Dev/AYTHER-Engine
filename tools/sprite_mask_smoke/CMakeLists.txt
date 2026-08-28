# sprite_mask_smoke (Vestuario) — el fragment shader de la máscara de tinte
# (sprite_mask.frag) EJECUTADO en la GPU y comparado por píxel contra la
# referencia CPU de su fórmula: out = c · mix(luma(t), t, m). Sin ROM ni
# grabación: dos PNGs sintéticos (asset de color pleno + máscara mitad negra /
# mitad blanca) y una FrameView armada a mano con solo la capa SpritesHd.
# Requiere GPU (ventana SDL oculta) — detrás de AYTHER_BUILD_SPIKE, como el
# resto de los oráculos de readback (molde: video_shader_smoke #378).
add_executable(sprite_mask_smoke main.cpp)
target_link_libraries(sprite_mask_smoke PRIVATE
    ayther_engine
    ayther_core
    SDL3::SDL3
    Vulkan::Vulkan
    ${CMAKE_DL_LIBS}
)
if(WIN32)
    target_link_libraries(sprite_mask_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(sprite_mask_smoke PRIVATE cxx_std_20)
target_compile_definitions(sprite_mask_smoke PRIVATE AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(sprite_mask_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Entra al ctest: no necesita ROM ni grabación — solo una GPU, y
# AYTHER_BUILD_SPIKE ya es un flag de máquina de desarrollo.
add_test(NAME sprite_mask COMMAND sprite_mask_smoke)
set_tests_properties(sprite_mask PROPERTIES
    TIMEOUT 120
    LABELS  "engine;native;gpu")
