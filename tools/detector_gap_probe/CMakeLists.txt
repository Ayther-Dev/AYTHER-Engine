# ---------------------------------------------------------------------------
# detector_gap_probe — cruza las ventanas del detector de eventos contra el
# bus crudo FM/PSG (#282), ver main.cpp. Built only when -DAYTHER_BUILD_SPIKE=ON.
# ---------------------------------------------------------------------------

add_executable(detector_gap_probe main.cpp)

target_link_libraries(detector_gap_probe PRIVATE
    ayther_engine
    ayther_core
    ${CMAKE_DL_LIBS}
)

if(WIN32)
    target_link_libraries(detector_gap_probe PRIVATE ws2_32 userenv bcrypt ntdll)
endif()

target_compile_features(detector_gap_probe PRIVATE cxx_std_20)

set_target_properties(detector_gap_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
