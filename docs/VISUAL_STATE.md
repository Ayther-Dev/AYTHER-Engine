# Visual state

**Status:** implemented; format 1.0

**Last verified:** 2026-10-03

The core savestate restores the emulated game, but not what the Engine
carries from one frame to the next in order to draw the following ones:
in-betweens in progress, recognition hysteresis, animation clocks and so on.
Restoring only the core makes a checkpoint present a different frame from the
linear run. The visual state is that missing part. A host exports it next to
the core state and restores both to reproduce the frame exactly.

The format lives in the installed header `ayther/engine/visual_state.hpp`,
namespace `ayther::engine::visual_state`. Its version is
`kVisualStateVersion` (1.0).

## API

| Declaration | Purpose |
| --- | --- |
| `AytherSession::game_state_identity()` | SHA-256 of the current core savestate, in lowercase hex. Empty when the core cannot serialize. |
| `AytherSession::export_visual_state()` | The visual state of the current frame and core state. |
| `AytherSession::restore_visual_state(state, expected_identity, expected_frame)` | Validates and restores a state exported for that identity and frame. |
| `validate_visual_state_header(header, identity, frame)` | The header checks alone, without touching a session. |

A `VisualState` is a header (version, game state identity, emulation frame,
section mask and script state) and a payload. The payload holds the sections
in increasing bit order, each one as a `u32` section bit, a `u32` size (both
little-endian) and its body.

## Sections

Every section is required (`kVisualStateRequiredSections`).

| Section | Contents |
| --- | --- |
| `sprite_tweens` | In-betweens in progress per instance |
| `screen_recognition` | Picture recognition hysteresis |
| `level_camera` | Level camera and panorama vote continuity |
| `palette_luma` | Per-palette luminance peak (the E1 tint reference) |
| `previous_audio_mask` | Channel mask applied to the last frame |
| `palette_signature` | CRAM stability and latched signatures |
| `animation_grouper` | SAT slot histories and animation groups |
| `plane_sequence_clocks` | Plane animation clocks |
| `cinematic` | Kinematic in progress, with its video and audio |
| `hd_animation_phase` | Phase of each level-1 HD animation |
| `panorama_tint` | Anchored tint reference of each panorama |

The Lua state of the pack's scripts is not part of the payload. A pack whose
scripts run every frame (`ayther.on_frame`) exports `script_state =
not_exportable`: its takes are recovered by simulating from their initial
state instead.

## Restore order

1. Restore the core state: `AytherSession::unserialize`.
2. Restore the visual state with the identity and frame recorded at export.
   On success the session is at that frame.
3. Restore the HD audio state (`restore_audio_hd_detector_windows`,
   `restore_audio_hd_voices` and `restore_audio_hd_requests_pending`), which is
   validated against the session's frame. See
   [Audio observation](AUDIO_OBSERVATION.md).

After a restore or a resume, the first frames that reach the device report
where they start on the device line through `audio_frame_output_boundary`
(see [Audio observation](AUDIO_OBSERVATION.md)). A host compares that position
with the presentation of the same frame to measure the image/audio offset.

## Restore codes

Everything is validated before anything changes: an incompatible state
returns its code and leaves the session untouched. The header checks run in
this order.

| Code | Meaning |
| --- | --- |
| `restored` | The state was applied. |
| `unsupported_version` | The format version is not 1.0. |
| `identity_mismatch` | The identity is empty, too long or not the expected one. |
| `frame_mismatch` | The state belongs to another frame. |
| `missing_sections` | A required section is absent; `missing_sections` lists them. |
| `unknown_sections` | The state carries sections this version does not know; `unknown_sections` lists them. |
| `invalid_payload` | The payload is malformed or exceeds `kVisualStatePayloadLimit`. |

## Verification

- `tests/contracts/visual_state_header.cpp`: the header compiles on its own
  and its constants are stable.
- `tests/unit/visual_state_codec_test.cpp`: the codec and every rejection.
- `tests/integration/visual_state_session_test.cpp`: export and restore on a
  real session.
- `tests/integration/visual_state_roundtrip_all_frames.cpp`: a restore at
  every frame of a take produces the same frames as the linear run.
- `tests/integration/audio_pending_midtake_test.cpp`: the restore order with
  the HD audio state, exported in the middle of a take.
