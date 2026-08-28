# ---------------------------------------------------------------------------
# video_stream_smoke — oráculo del video por rango desde el pack (#375).
#
# Se sintetiza su propio material y hornea su propio pack, así que corre entero
# en el ctest sin ffmpeg ni assets del proyecto. Con un `.ivf` all-intra real
# como argumento agrega la comparación de PÍXELES.
#
# Sólo existe con libvpx construido (ver el CMakeLists raíz).
# ---------------------------------------------------------------------------

add_executable(video_stream_smoke main.cpp)

target_link_libraries(video_stream_smoke PRIVATE
    ayther_engine
    ayther_core       # PackBuilder + AyArchive por FFI
    ${CMAKE_DL_LIBS}
)

if(WIN32)
    target_link_libraries(video_stream_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()

target_compile_features(video_stream_smoke PRIVATE cxx_std_20)

set_target_properties(video_stream_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

add_test(NAME video_stream COMMAND video_stream_smoke)
set_tests_properties(video_stream PROPERTIES
    TIMEOUT 120
    LABELS  "engine;native;integration;vpx")
