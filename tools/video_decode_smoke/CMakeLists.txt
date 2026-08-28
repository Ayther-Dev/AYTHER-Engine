# ---------------------------------------------------------------------------
# video_decode_smoke — oráculo de ayther::VideoClip (#263). Ver main.cpp.
#
# Linkea el engine (no libvpx directo): lo que se prueba es la clase del motor,
# con su pimpl, no la librería de terceros — eso ya lo cubre vpx_smoke.
#
# Sólo existe con libvpx construido (ver el CMakeLists raíz).
# ---------------------------------------------------------------------------

add_executable(video_decode_smoke main.cpp)

target_link_libraries(video_decode_smoke PRIVATE
    ayther_engine
    ayther_core       # símbolos del FFI que el engine referencia
    ${CMAKE_DL_LIBS}
)

if(WIN32)
    target_link_libraries(video_decode_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()

target_compile_features(video_decode_smoke PRIVATE cxx_std_20)

set_target_properties(video_decode_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Sin argumentos verifica lo que no necesita material y sale verde — el ctest
# tiene que correr en una máquina sin ffmpeg. El material se le pasa a mano
# cuando se quiere el chequeo completo (acceso aleatorio + costo).
add_test(NAME video_decode COMMAND video_decode_smoke)
set_tests_properties(video_decode PROPERTIES
    TIMEOUT 120
    LABELS  "engine;native;integration;vpx")
