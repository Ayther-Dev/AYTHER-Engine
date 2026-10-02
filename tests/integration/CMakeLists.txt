# ---------------------------------------------------------------------------
# Session, audio, recording, and pack documents: suites that drive compiled
# engine subsystems rather than a single header. Several open an SDL audio
# device under the dummy driver or write temporary files, which is why
# ayther_add_native_test gives each one an isolated working directory.
# ---------------------------------------------------------------------------

foreach(_source IN ITEMS
        audio_asset_fallback_test.cpp
        audio_level_test.cpp
        audio_live_resume_test.cpp
        audio_match_identity_test.cpp
        audio_player_pause_test.cpp
        audio_tail_test.cpp
        audio_unified_mix_test.cpp
        ayther_recording_test.cpp
        elements_toml_test.cpp
        gain_source_test.cpp
        plane_hash_variants_test.cpp)
    ayther_add_native_test("${_source}" integration)
endforeach()

# __FILE__ is deliberately remapped in reproducible release builds, so it
# cannot be used to find checkout fixtures at runtime.
target_compile_definitions(ayther_elements_toml_test PRIVATE
    "AYTHER_RF18_SCHEMA1_FIXTURE=\"${PROJECT_SOURCE_DIR}/tests/fixtures/rf18/fixtures/audio-continuity-schema1.toml\"")

add_executable(ayther_repeat_recording_fixture repeat_recording_fixture.cpp)
target_compile_features(ayther_repeat_recording_fixture PRIVATE cxx_std_20)
target_link_libraries(ayther_repeat_recording_fixture PRIVATE Ayther::engine)
