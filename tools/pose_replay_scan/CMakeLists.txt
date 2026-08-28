# pose_replay_scan — sonda de diagnóstico del reemplazo de poses durante un
# replay: barre un rango de frames con el pose-set del proyecto cargado y
# reporta subs aplicados, por qué una pose NO matchea (miembro faltante y dónde
# quedó su hash) y qué sprites sueltos caen suprimidos por la región HD.
# Built only when -DAYTHER_BUILD_SPIKE=ON. ROM via AYTHER_PROBE_ROM.
add_executable(pose_replay_scan main.cpp)
target_link_libraries(pose_replay_scan PRIVATE ayther_engine ayther_core ${CMAKE_DL_LIBS})
if(WIN32)
    target_link_libraries(pose_replay_scan PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(pose_replay_scan PRIVATE cxx_std_20)
target_compile_definitions(pose_replay_scan PRIVATE AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(pose_replay_scan PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
