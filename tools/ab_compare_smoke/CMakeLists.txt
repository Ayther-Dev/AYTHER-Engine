# ab_compare_smoke (#305) — la imagen de COMPARACION del preview A/B verificada
# por pixel: la copia es identica al offscreen que la origino, re-renderizar
# encima no la toca, y volver a capturar la actualiza. Sin ROM ni grabacion (la
# FrameView se arma a mano, mismo criterio que video_shader_smoke #378), asi que
# entra al ctest — lo unico que pide es una GPU.
add_executable(ab_compare_smoke main.cpp)
target_link_libraries(ab_compare_smoke PRIVATE
    ayther_engine
    ayther_core
    SDL3::SDL3
    Vulkan::Vulkan
    ${CMAKE_DL_LIBS}
)
if(WIN32)
    target_link_libraries(ab_compare_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(ab_compare_smoke PRIVATE cxx_std_20)
target_compile_definitions(ab_compare_smoke PRIVATE AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(ab_compare_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_test(NAME ab_compare COMMAND ab_compare_smoke)

# Necesita una GPU de verdad (crea un VulkanTestContext), asi que en una maquina sin
# Vulkan —los runners de CI, por ejemplo— hay que poder dejarlo afuera:
# ctest -LE gpu corre todo lo demas. Etiquetar es preferible a excluir por
# nombre, que se desactualiza en silencio cuando se agrega el siguiente.
set_tests_properties(ab_compare PROPERTIES
    TIMEOUT 120
    LABELS  "engine;native;gpu")
