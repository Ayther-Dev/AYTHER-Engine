# replay_keys_probe (#492) — ¿el playback secuencial puebla replay_keys? Sin GPU.
# Built only when -DAYTHER_BUILD_SPIKE=ON. ROM via AYTHER_PROBE_ROM.
add_executable(replay_keys_probe main.cpp)
target_link_libraries(replay_keys_probe PRIVATE ayther_engine ayther_core ${CMAKE_DL_LIBS})
if(WIN32)
    target_link_libraries(replay_keys_probe PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(replay_keys_probe PRIVATE cxx_std_20)
target_compile_definitions(replay_keys_probe PRIVATE AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(replay_keys_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
