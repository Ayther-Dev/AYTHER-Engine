# ---------------------------------------------------------------------------
# Pure logic and header-only components. No SDL device, no filesystem, no
# emulator core: every input is constructed in the test itself.
# ---------------------------------------------------------------------------

foreach(_source IN ITEMS
        animation_phase_test.cpp
        audio_bus_balance_test.cpp
        capabilities_test.cpp
        audio_fade_test.cpp
        audio_match_rule_test.cpp
        audio_playback_policy_test.cpp
        audio_sequence_lifetime_test.cpp
        audio_seq_anchor_test.cpp
        cram_palette_test.cpp
        core_path_test.cpp
        decode_limits_test.cpp
        failure_escalation_test.cpp
        frame_composability_test.cpp
        log_test.cpp
        loop_region_test.cpp
        music_sequence_model_test.cpp
        music_sequence_validation_test.cpp
        music_event_normalization_test.cpp
        music_event_scoring_test.cpp
        music_pattern_analysis_test.cpp
        music_proposal_assessment_test.cpp
        music_analysis_snapshot_test.cpp
        music_analysis_limits_test.cpp
        music_analysis_determinism_test.cpp
        music_corpus_quality_test.cpp
        music_pack_contract_test.cpp
        music_identity_state_test.cpp
        music_causal_recognizer_test.cpp
        music_recognition_candidate_test.cpp
        music_late_entry_test.cpp
        music_transition_arbiter_test.cpp
        music_live_recognition_test.cpp
        music_position_recovery_test.cpp
        music_golden_axe_battle_authorization_test.cpp
        music_golden_axe_traversal_test.cpp
        music_recognition_load_test.cpp
        music_asset_owner_test.cpp
        music_audio_math_test.cpp
        music_transition_span_test.cpp
        music_source_lifecycle_test.cpp
        music_transition_queue_test.cpp
        music_transition_cancel_test.cpp
        music_pause_signal_test.cpp
        music_pause_state_test.cpp
        music_pause_cycles_test.cpp
        music_voice_budget_test.cpp
        output_profile_test.cpp
        pack_api_test.cpp
        pack_runtime_test.cpp
        pano_bands_test.cpp
        panorama_cover_test.cpp
        parallax_bands_test.cpp
        plane_set_match_test.cpp
        plane_set_order_test.cpp
        plane_bands_test.cpp
        plane_set_split_test.cpp
        plane_sub_join_test.cpp
        pose_anchor_order_test.cpp
        pose_depth_test.cpp
        recording_controller_test.cpp
        reference_compositor_test.cpp
        render_image_view_test.cpp
        render_observation_builder_test.cpp
        render_probe_json_test.cpp
        render_probe_o1_test.cpp
        render_probe_missing_test.cpp
        render_probe_o2_test.cpp
        runtime_options_test.cpp
        sprite_line_limits_test.cpp
        texture_residency_test.cpp
        vdp_sprite_order_test.cpp
        visual_state_codec_test.cpp
        widescreen_test.cpp)
    ayther_add_native_test("${_source}" unit)
endforeach()

target_compile_definitions(ayther_music_corpus_quality_test PRIVATE
    AYTHER_RF18_EVIDENCE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/rf18/evidence")

target_compile_definitions(ayther_music_pause_cycles_test PRIVATE
    AYTHER_RF18_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}")

target_compile_definitions(ayther_capabilities_test PRIVATE
    AYTHER_EXPECTED_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    AYTHER_EXPECTED_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    AYTHER_EXPECTED_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    AYTHER_EXPECTED_VULKAN=$<BOOL:${AYTHER_BUILD_RENDERER}>)

set_tests_properties(ayther.unit.capabilities_test PROPERTIES
    ENVIRONMENT
        "DISPLAY=;WAYLAND_DISPLAY=;SDL_VIDEODRIVER=dummy;VK_ICD_FILENAMES=__ayther_missing_icd__")
