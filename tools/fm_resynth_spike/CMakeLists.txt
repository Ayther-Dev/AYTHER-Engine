# ---------------------------------------------------------------------------
# fm_resynth_spike — el GATE de la Fase 0 del router por voz (#327), ver main.cpp.
# Built only when -DAYTHER_BUILD_SPIKE=ON.
# ---------------------------------------------------------------------------

add_executable(fm_resynth_spike main.cpp)

target_link_libraries(fm_resynth_spike PRIVATE
    ayther_engine
    ayther_core
    ayther::ymfm
    ${CMAKE_DL_LIBS}
)

if(WIN32)
    target_link_libraries(fm_resynth_spike PRIVATE ws2_32 userenv bcrypt ntdll)
endif()

target_compile_features(fm_resynth_spike PRIVATE cxx_std_20)

set_target_properties(fm_resynth_spike PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
