# enhance_shader_smoke (#504) — oráculo GPU del filtro «Mejorar por software»
# (#493): indexed_plane.frag (rama enhance) contra la referencia CPU xbr_ref.h,
# píxel a píxel, sin ROM (VkIndexedPlane directo sobre un target de 48×48).
# Built only when -DAYTHER_BUILD_SPIKE=ON. Requiere GPU (ctest label gpu).
add_executable(enhance_shader_smoke main.cpp)
target_link_libraries(enhance_shader_smoke PRIVATE
    ayther_engine
    ayther_core
    SDL3::SDL3
    Vulkan::Vulkan
    ${CMAKE_DL_LIBS}
)
if(WIN32)
    target_link_libraries(enhance_shader_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(enhance_shader_smoke PRIVATE cxx_std_20)
target_compile_definitions(enhance_shader_smoke PRIVATE AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(enhance_shader_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
add_test(NAME enhance_shader COMMAND enhance_shader_smoke)
set_tests_properties(enhance_shader PROPERTIES
    TIMEOUT 120
    LABELS  "engine;native;gpu")
