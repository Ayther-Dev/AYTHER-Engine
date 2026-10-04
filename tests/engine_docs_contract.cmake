# Spec 002 (BR-178, RNF-8): the documentation says what the public surface
# does. The render observation (C3) and visual state (C4) APIs, the replacement
# composition rules R1-R9 and the changelog must name the declarations a host
# uses, and the documentation index must link the new documents.

cmake_minimum_required(VERSION 3.21)

if(NOT AYTHER_ROOT)
    message(FATAL_ERROR "Missing -DAYTHER_ROOT")
endif()

set(_violations "")

# _require(<file relative to AYTHER_ROOT> <token>...)
function(_require file)
    set(_path "${AYTHER_ROOT}/${file}")
    if(NOT EXISTS "${_path}")
        set(_violations "${_violations};${file} is missing" PARENT_SCOPE)
        return()
    endif()
    file(READ "${_path}" _text)
    set(_local "${_violations}")
    foreach(_token IN LISTS ARGN)
        string(FIND "${_text}" "${_token}" _at)
        if(_at EQUAL -1)
            list(APPEND _local "${file} does not mention `${_token}`")
        endif()
    endforeach()
    set(_violations "${_local}" PARENT_SCOPE)
endfunction()

_require(docs/RENDER_OBSERVATION.md
    "ayther/engine/render_observer.hpp"
    "Config::render_observer"
    "publish_render_observation"
    "last_draw_report"
    "on_render_frame"
    "contract_version"
    "max_occurrences"
    "occurrences_total"
    "replaced" "original_unassigned" "assigned_not_applied" "hidden_by_author"
    "raster_split" "fade" "line_hscroll" "column_vscroll"
    "texture_pending" "texture_failed" "frame_not_composable" "member_hidden"
    "hd_off"
    "in_pass" "partitioned" "lane" "discarded")

_require(docs/VISUAL_STATE.md
    "ayther/engine/visual_state.hpp"
    "game_state_identity"
    "export_visual_state"
    "restore_visual_state"
    "not_exportable"
    "sprite_tweens" "screen_recognition" "level_camera" "palette_luma"
    "previous_audio_mask" "palette_signature" "animation_grouper"
    "plane_sequence_clocks" "cinematic" "hd_animation_phase" "panorama_tint"
    "unsupported_version" "identity_mismatch" "frame_mismatch"
    "missing_sections" "unknown_sections" "invalid_payload"
    "restore_audio_hd_requests_pending")

_require(docs/REPLACEMENT_COMPOSITION.md
    "### R1" "### R2" "### R3" "### R4" "### R5" "### R6" "### R7" "### R8"
    "### R9"
    "set_synchronous_textures"
    "prewarm_textures"
    "catalog_texture_assets"
    "evict_pack_textures"
    "pose.asset_missing"
    "frame_not_composable")

_require(docs/README.md
    "(RENDER_OBSERVATION.md)"
    "(VISUAL_STATE.md)"
    "(REPLACEMENT_COMPOSITION.md)")

_require(CHANGELOG.md
    "render_observer.hpp"
    "visual_state.hpp"
    "render_probe"
    "docs/REPLACEMENT_COMPOSITION.md"
    "evict_pack_textures")

list(REMOVE_ITEM _violations "")
if(_violations)
    foreach(_violation IN LISTS _violations)
        message("  [FAIL] ${_violation}")
    endforeach()
    message(FATAL_ERROR "The spec 002 documentation is incomplete")
endif()

message("  [ OK ] the spec 002 documentation covers C3, C4, R1-R9 and the changelog")
