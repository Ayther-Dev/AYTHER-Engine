# pose_frame_probe — sonda de diagnóstico de sprites/planos de un frame. -DAYTHER_BUILD_SPIKE=ON.
add_executable(pose_frame_probe main.cpp)
target_link_libraries(pose_frame_probe PRIVATE ayther_engine ayther_core ${CMAKE_DL_LIBS})
if(WIN32)
    target_link_libraries(pose_frame_probe PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(pose_frame_probe PRIVATE cxx_std_20)
target_compile_definitions(pose_frame_probe PRIVATE AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(pose_frame_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
