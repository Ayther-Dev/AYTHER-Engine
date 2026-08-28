# ---------------------------------------------------------------------------
# mute_silence_probe — mide el PCM residual con «todo silenciado», ver main.cpp.
# Built only when -DAYTHER_BUILD_SPIKE=ON.
# ---------------------------------------------------------------------------

add_executable(mute_silence_probe main.cpp)

target_link_libraries(mute_silence_probe PRIVATE
    ayther_engine
    ayther_core
    ${CMAKE_DL_LIBS}
)

if(WIN32)
    target_link_libraries(mute_silence_probe PRIVATE ws2_32 userenv bcrypt ntdll)
endif()

target_compile_features(mute_silence_probe PRIVATE cxx_std_20)

set_target_properties(mute_silence_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
