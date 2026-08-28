# group_hide_render_smoke (2026-07-31) — ocultar apaga el HD, EN PÍXELES: con la
# capa Sprites apagada (ojo del header del timeline) el frame con poses HD es
# idéntico al mismo frame sin poses, y ocultar un miembro de la pose (ojo del
# Inspector) desactiva el asset del grupo — que en píxeles es lo mismo que no
# tener asset. Requiere GPU (ventana SDL oculta).
# Built only when -DAYTHER_BUILD_SPIKE=ON. ROM via AYTHER_PROBE_ROM.
add_executable(group_hide_render_smoke main.cpp)
target_link_libraries(group_hide_render_smoke PRIVATE
    ayther_engine
    ayther_core
    SDL3::SDL3
    Vulkan::Vulkan
    ${CMAKE_DL_LIBS}
)
if(WIN32)
    target_link_libraries(group_hide_render_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(group_hide_render_smoke PRIVATE cxx_std_20)
target_compile_definitions(group_hide_render_smoke PRIVATE AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(group_hide_render_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
