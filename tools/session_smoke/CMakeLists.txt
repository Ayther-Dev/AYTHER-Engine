# ---------------------------------------------------------------------------
# session_smoke — headless runtime validation of AytherSession (R2.3 de-risk).
# Unlike the determinism_spike (RetroRunner only), this links the full motor
# (ayther_engine) + the Rust core, so it exercises the real step() pipeline.
# See docs/CPP_API_REFERENCE.md#primary-session-contract.
#
# Built only when -DAYTHER_BUILD_SPIKE=ON.
# ---------------------------------------------------------------------------

add_executable(session_smoke smoke.cpp)

target_link_libraries(session_smoke PRIVATE
    ayther_engine     # AytherSession + RetroRunner + AudioPlayer (SDL3 PUBLIC)
    ayther_core       # Rust FFI symbols the pipeline calls (resolved here)
    ${CMAKE_DL_LIBS}
)

if(WIN32)
    # Windows system libs required by the Rust standard library.
    target_link_libraries(session_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()

target_compile_features(session_smoke PRIVATE cxx_std_20)

set_target_properties(session_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
