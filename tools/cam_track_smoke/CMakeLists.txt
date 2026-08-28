# ---------------------------------------------------------------------------
# cam_track_smoke — valida la cámara en espacio de NIVEL de EM-1 (#226):
# unwrap secuencial de scroll en FrameView.plane_cam_*. Lee
# tests/test_config.toml. Built only when -DAYTHER_BUILD_SPIKE=ON.
# ---------------------------------------------------------------------------

add_executable(cam_track_smoke main.cpp)

target_link_libraries(cam_track_smoke PRIVATE
    ayther_engine
    ayther_core
    ${CMAKE_DL_LIBS}
)

if(WIN32)
    target_link_libraries(cam_track_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()

target_compile_features(cam_track_smoke PRIVATE cxx_std_20)
target_compile_definitions(cam_track_smoke PRIVATE
    AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

set_target_properties(cam_track_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
