# ---------------------------------------------------------------------------
# hd_luma_smoke — E1 (Estados de una Pose): verifica sprite_sub_luma por-sub con una sustitución HD de
# una paleta a lo largo de una toma (base del factor de fundido para el
# reemplazo HD). Lee tests/test_config.toml. Built only when -DAYTHER_BUILD_SPIKE=ON.
# ---------------------------------------------------------------------------

add_executable(hd_luma_smoke main.cpp)

target_link_libraries(hd_luma_smoke PRIVATE
    ayther_engine
    ayther_core
    ${CMAKE_DL_LIBS}
)

if(WIN32)
    target_link_libraries(hd_luma_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()

target_compile_features(hd_luma_smoke PRIVATE cxx_std_20)
target_compile_definitions(hd_luma_smoke PRIVATE
    AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

set_target_properties(hd_luma_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
