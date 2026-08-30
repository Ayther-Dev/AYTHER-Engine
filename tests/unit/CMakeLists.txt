# ---------------------------------------------------------------------------
# Pure logic and header-only components. No SDL device, no filesystem, no
# emulator core: every input is constructed in the test itself.
# ---------------------------------------------------------------------------

foreach(_source IN ITEMS
        audio_bus_balance_test.cpp
        audio_fade_test.cpp
        audio_match_rule_test.cpp
        audio_seq_anchor_test.cpp
        cram_palette_test.cpp
        decode_limits_test.cpp
        failure_escalation_test.cpp
        log_test.cpp
        loop_region_test.cpp
        output_profile_test.cpp
        pack_runtime_test.cpp
        pano_bands_test.cpp
        panorama_cover_test.cpp
        parallax_bands_test.cpp
        recording_controller_test.cpp
        runtime_options_test.cpp
        widescreen_test.cpp)
    ayther_add_native_test("${_source}" unit)
endforeach()
