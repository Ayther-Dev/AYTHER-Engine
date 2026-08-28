# recompose_spike (#270) — spike de fidelidad del render propio: recompone la
# pantalla desde el estado FINAL del VDP (export ayther_recompose_frame del
# fork, mismo renderer del core) y la compara píxel a píxel contra el
# framebuffer que devolvió el emulador ese mismo frame, por cada frame de una
# toma. Reporta % idéntico por pase (base + comportamientos del VDP apagados
# de a uno), concentración por scanline y trípticos PNG de los peores frames.
# Built only when -DAYTHER_BUILD_SPIKE=ON. ROM via AYTHER_PROBE_ROM.
add_executable(recompose_spike main.cpp)
target_link_libraries(recompose_spike PRIVATE ayther_engine ayther_core ${CMAKE_DL_LIBS})
if(WIN32)
    target_link_libraries(recompose_spike PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(recompose_spike PRIVATE cxx_std_20)
target_compile_definitions(recompose_spike PRIVATE AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(recompose_spike PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
