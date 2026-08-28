# ---------------------------------------------------------------------------
# seq_mute_probe — valida el mute selectivo por firma de AudioSeqSub (ver
# main.cpp). Built only when -DAYTHER_BUILD_SPIKE=ON.
# ---------------------------------------------------------------------------

add_executable(seq_mute_probe main.cpp)

target_link_libraries(seq_mute_probe PRIVATE
    ayther_engine
    ayther_core
    ${CMAKE_DL_LIBS}
)

if(WIN32)
    target_link_libraries(seq_mute_probe PRIVATE ws2_32 userenv bcrypt ntdll)
endif()

target_compile_features(seq_mute_probe PRIVATE cxx_std_20)

set_target_properties(seq_mute_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
