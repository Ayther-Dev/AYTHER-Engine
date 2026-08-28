# video_plane_smoke (#479) — el video de la Cinematica dibujado en el z del
# plano mas alto de SU mascara, verificado por pixel: con mascara el Sprite vivo
# queda ENCIMA del video; sin mascara el video lo tapa (el defecto que #479
# arregla). El par es lo que hace al oraculo no vacio.
# Sin ROM ni grabacion: la escena se sintetiza (VRAM, CRAM y dos SceneElement).
# Requiere GPU (ventana SDL oculta) — como el resto de los oraculos de readback.
add_executable(video_plane_smoke main.cpp)
target_link_libraries(video_plane_smoke PRIVATE
    ayther_engine
    ayther_core
    SDL3::SDL3
    Vulkan::Vulkan
    ${CMAKE_DL_LIBS}
)
if(WIN32)
    target_link_libraries(video_plane_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(video_plane_smoke PRIVATE cxx_std_20)
target_compile_definitions(video_plane_smoke PRIVATE AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(video_plane_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Entra al ctest como video_shader: no necesita ROM ni grabacion, solo GPU.
add_test(NAME video_plane COMMAND video_plane_smoke)
set_tests_properties(video_plane PROPERTIES
    TIMEOUT 120
    LABELS  "engine;native;gpu")
