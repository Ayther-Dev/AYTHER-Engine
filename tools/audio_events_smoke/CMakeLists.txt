# ---------------------------------------------------------------------------
# audio_events_smoke — valida el wiring de Audios C-A2 (deteccion de eventos +
# ventanas de sustitucion por evento) a nivel AytherSession, contra Sonic 2.
# Built only when -DAYTHER_BUILD_SPIKE=ON.
# ---------------------------------------------------------------------------

add_executable(audio_events_smoke main.cpp)

target_link_libraries(audio_events_smoke PRIVATE
    ayther_engine
    ayther_core
    ${CMAKE_DL_LIBS}
)

if(WIN32)
    target_link_libraries(audio_events_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()

target_compile_features(audio_events_smoke PRIVATE cxx_std_20)
target_compile_definitions(audio_events_smoke PRIVATE
    AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

set_target_properties(audio_events_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
