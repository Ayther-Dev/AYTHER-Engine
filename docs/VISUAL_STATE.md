# Visual state

**Status:** implemented; format 1.0; both emulation providers

**Last verified:** 2026-11-19

The emulator's saved state restores the emulated game, but not what the Engine
carries from one frame to the next in order to draw the following ones:
in-betweens in progress, recognition hysteresis, animation clocks and so on.
Restoring only the emulator makes a checkpoint present a different frame from
the linear run. The visual state is that missing part. A host exports it next
to the emulator state and restores both to reproduce the frame exactly.

The format lives in the installed header `ayther/engine/visual_state.hpp`,
namespace `ayther::engine::visual_state`. Its version is
`kVisualStateVersion` (1.0).

## API

| Declaration | Purpose |
| --- | --- |
| `AytherSession::game_state_identity()` | SHA-256 of the current emulator state, in lowercase hex. Empty when the state cannot be serialized. |
| `AytherSession::export_visual_state()` | The visual state of the current frame and emulator state. |
| `AytherSession::restore_visual_state(state, expected_identity, expected_frame)` | Validates and restores a state exported for that identity and frame. |
| `validate_visual_state_header(header, identity, frame)` | The header checks alone, without touching a session. |

A `VisualState` is a header (version, game state identity, emulation frame,
section mask and script state) and a payload. The payload holds the sections
in increasing bit order, each one as a `u32` section bit, a `u32` size (both
little-endian) and its body.

### Providers

The export, restore and section layout are the same on both providers. Only
the input of `game_state_identity()` differs: on `libretro` it hashes the
core's `retro_serialize` state; on `tessera_native` it hashes the native
checkpoint (`NativeEmulation::checkpoint`). An identity is therefore valid only
for the provider that produced it, and a visual state exported on one provider
is rejected with `identity_mismatch` on the other.

## Sections

Every section is required (`kVisualStateRequiredSections`, `0x7FF`).

| Bit | Section | Contents |
| --- | --- | --- |
| 0 | `sprite_tweens` | In-betweens in progress per instance |
| 1 | `screen_recognition` | Picture recognition hysteresis |
| 2 | `level_camera` | Level camera and panorama vote continuity |
| 3 | `palette_luma` | Per-palette luminance peak (the E1 tint reference) |
| 4 | `previous_audio_mask` | Channel mask applied to the last frame |
| 5 | `palette_signature` | CRAM stability and latched signatures |
| 6 | `animation_grouper` | SAT slot histories and animation groups |
| 7 | `plane_sequence_clocks` | Plane animation clocks |
| 8 | `cinematic` | Kinematic in progress, with its video and audio |
| 9 | `hd_animation_phase` | Phase of each level-1 HD animation |
| 10 | `panorama_tint` | Anchored tint reference of each panorama |

The Lua state of the pack's scripts is not part of the payload. A pack whose
scripts run every frame (`ayther.on_frame`) exports `script_state =
not_exportable`: its takes are recovered by simulating from their initial
state instead.

## Restore order

1. Restore the emulator state: `AytherSession::unserialize`.
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
| `identity_mismatch` | The identity is empty, longer than `kVisualStateIdentityLimit` (256 bytes) or not the expected one. |
| `frame_mismatch` | The state belongs to another frame. |
| `missing_sections` | A required section is absent; `missing_sections` lists them. |
| `unknown_sections` | The state carries sections this version does not know; `unknown_sections` lists them. |
| `invalid_payload` | The payload is malformed, exceeds `kVisualStatePayloadLimit` (64 MiB), or names a panorama the session does not define. |

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

These tests run on the libretro provider. On `tessera_native`, the checkpoint
path that `game_state_identity()` hashes is covered by
`tests/integration/tessera_checkpoint_test.cpp` and
`tests/integration/tessera_native_state.cpp` (rewind and takes); there is no
visual-state round trip on that provider yet.
