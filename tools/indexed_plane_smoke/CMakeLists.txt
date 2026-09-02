# indexed_plane_smoke (#271) — oráculo del pipeline indexado: dibuja el plano B
# con VkIndexedPlane (VRAM como índices + CRAM como paleta, en GPU) y lo compara
# BIT A BIT contra la recomposición del mismo plano por el renderer del core
# (ayther_recompose_frame con la máscara "solo B"). El único tool que crea un
# contexto Vulkan: ventana SDL oculta (VulkanTestContext no tiene camino headless).
# Built only when -DAYTHER_BUILD_SPIKE=ON. ROM via AYTHER_PROBE_ROM. Requiere GPU.
add_executable(indexed_plane_smoke main.cpp)
target_link_libraries(indexed_plane_smoke PRIVATE
    ayther_engine
    ayther_core
    SDL3::SDL3
    Vulkan::Vulkan
    ${CMAKE_DL_LIBS}
)
if(WIN32)
    target_link_libraries(indexed_plane_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(indexed_plane_smoke PRIVATE cxx_std_20)
target_compile_definitions(indexed_plane_smoke PRIVATE AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(indexed_plane_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
