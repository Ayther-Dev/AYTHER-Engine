# ---------------------------------------------------------------------------
# audio_event_smoke — valida el AudioEventDetector (C-A2) sobre datos reales:
# maneja el core (RetroRunner), alimenta el detector (FFI del core Rust) frame a
# frame, y verifica que el set de eventos sea replay-consistente. Linkea el motor
# + el core Rust (como session_smoke).
#
# Built only when -DAYTHER_BUILD_SPIKE=ON.
# ---------------------------------------------------------------------------

add_executable(audio_event_smoke smoke.cpp)

target_link_libraries(audio_event_smoke PRIVATE
    ayther_engine     # RetroRunner (+ AudioPlayer/SDL3)
    ayther_core       # FFI del detector (ayther_audio_event_*)
    ${CMAKE_DL_LIBS}
)

if(WIN32)
    target_link_libraries(audio_event_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()

target_compile_features(audio_event_smoke PRIVATE cxx_std_20)

set_target_properties(audio_event_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
