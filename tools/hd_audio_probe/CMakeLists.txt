# hd_audio_probe — sonda del issue #146 (degradacion de sonido con Assets HD):
# playback con y sin pose-overrides → compara estado final del core + ms/frame.
# Built only when -DAYTHER_BUILD_SPIKE=ON. ROM via AYTHER_PROBE_ROM.
add_executable(hd_audio_probe main.cpp)
target_link_libraries(hd_audio_probe PRIVATE ayther_engine ayther_core ${CMAKE_DL_LIBS})
if(WIN32)
    target_link_libraries(hd_audio_probe PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(hd_audio_probe PRIVATE cxx_std_20)
target_compile_definitions(hd_audio_probe PRIVATE AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(hd_audio_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
