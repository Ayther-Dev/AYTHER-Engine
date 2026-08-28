# ---------------------------------------------------------------------------
# render_output_smoke — el oráculo de que lo que se DIBUJA se ve, con una ROM
# SINTÉTICA (#415).
#
# Entra al ctest NORMAL, por el mismo motivo que audio_output_smoke: las otras
# veinte herramientas de render necesitan una ROM comercial, viven fuera de la
# corrida diaria y se ejecutan cuando alguien se acuerda. Ésta se fabrica su
# propio Mega Drive (68k escrito en el .cpp, ver tools/common/synth_rom.h), así
# que no hay licencia que discutir ni corpus que preparar.
#
# No pide GPU: mide el framebuffer del core y el inventario de escena, que es
# todo lo que el motor produce ANTES de subirlo a Vulkan. Si no está el core del
# fork, se saltea con un [skip] visible.
# ---------------------------------------------------------------------------
add_executable(render_output_smoke main.cpp)
target_link_libraries(render_output_smoke PRIVATE
    ayther_engine
    ayther_core
    SDL3::SDL3
    ${CMAKE_DL_LIBS}
)
if(WIN32)
    target_link_libraries(render_output_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(render_output_smoke PRIVATE cxx_std_20)
target_compile_definitions(render_output_smoke PRIVATE AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(render_output_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_test(NAME render_output COMMAND render_output_smoke)
# Igual que audio_output: sin el core del fork —que no viaja en el repo— no hay
# nada que medir, y un 0 lo contaría como «Passed». El 77 lo declara SALTEADO.
set_tests_properties(render_output PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT          120
    LABELS           "engine;native;integration;render")
