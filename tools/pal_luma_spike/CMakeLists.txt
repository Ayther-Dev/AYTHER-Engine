# ---------------------------------------------------------------------------
# pal_luma_spike — E1 (Estados de una Pose): valida la señal de LUMINANCIA de
# una paleta a lo largo de una toma (base del factor de fundido para el
# reemplazo HD). Lee tests/test_config.toml. Built only when -DAYTHER_BUILD_SPIKE=ON.
# ---------------------------------------------------------------------------

add_executable(pal_luma_spike main.cpp)

target_link_libraries(pal_luma_spike PRIVATE
    ayther_engine
    ayther_core
    ${CMAKE_DL_LIBS}
)

if(WIN32)
    target_link_libraries(pal_luma_spike PRIVATE ws2_32 userenv bcrypt ntdll)
endif()

target_compile_features(pal_luma_spike PRIVATE cxx_std_20)
target_compile_definitions(pal_luma_spike PRIVATE
    AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

set_target_properties(pal_luma_spike PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
