# AYTHER Engine — index of installed headers

> **GENERATED — do not edit by hand.** `pwsh tools/gen_api_reference.ps1`.
> Derived from `cmake/AytherPublicHeaders.txt`, the manifest that
> drives CMake's installed allowlist: if a header enters
> or leaves the surface, this page reflects it without anyone editing a
> parallel list.

The installed surface and its stability are described in
[`API_COMPATIBILITY.md`](API_COMPATIBILITY.md).
Appearing in this index does not by itself imply a stability guarantee.

## The 15 headers

| header | what it provides |
|---|---|
| [`audio_asset_level.h`](#audio_asset_levelh) | Installed public header. |
| [`audio_match_rule.h`](#audio_match_ruleh) | Installed public header. |
| [`ayther_animation.h`](#ayther_animationh) | Installed public header. |
| [`ayther_audio_events.h`](#ayther_audio_eventsh) | Installed public header. |
| [`ayther_core_ffi.h`](#ayther_core_ffih) | Installed public header. |
| [`ayther_layers.h`](#ayther_layersh) | Installed public header. |
| [`ayther_mode3.h`](#ayther_mode3h) | Installed public header. |
| [`ayther_result.h`](#ayther_resulth) | Installed public header. |
| [`ayther_sdk_version.h`](#ayther_sdk_versionh) | Installed public header. |
| [`ayther_sdk.h`](#ayther_sdkh) | Installed public header. |
| [`ayther_session.h`](#ayther_sessionh) | Installed public header. |
| [`ayther_version.h`](#ayther_versionh) | Installed public header. |
| [`engine/capabilities.hpp`](#enginecapabilitieshpp) | Installed public header. |
| [`engine/engine.hpp`](#engineenginehpp) | Installed public header. |
| [`log.h`](#logh) | Installed public header. |

---

<a id="audio_asset_levelh"></a>

## audio_asset_level.h

audio_asset_level.h — measured level of a decoded audio asset.

`AudioPlayer` produces this value from decoded PCM and `AytherSession`
exposes it to authoring clients. Keeping the value type independent avoids a
cyclic include between those components.

Values answer three pre-publication questions: whether the asset clips,
whether it will be masked by native audio, and which non-destructive gain
adjustment is appropriate.

**Declares:** `AudioAssetLevel`, `ayther`

_The installed header (`include/ayther/audio_asset_level.h`) carries the full documentation of every symbol._

---

<a id="audio_match_ruleh"></a>

## audio_match_rule.h

audio_match_rule.h — event-substitution matching policy.

Exact signatures distinguish note, channel, and pan changes. An author can
opt into broader matching per assignment, persisted as `match` in
audio_events.toml. Legacy packs remain exact without migration because the
persisted primary key is still `signature`.

This header has no SDL dependency. Its deterministic table lookup is covered
by tests/audio_match_rule_test.cpp.

**Declares:** `AudioMatchIndex`, `AudioMatchRuleInfo`, `ayther`, `clear`, `Entry`, `uint8_t`

_The installed header (`include/ayther/audio_match_rule.h`) carries the full documentation of every symbol._

---

<a id="ayther_animationh"></a>

## ayther_animation.h

ayther_animation.h — C-S2: play an HD animation **in phase** with the game.

## What this is

An action is an ordered set of timed poses (AnimationClip, C-S1). C-S2
draws it in HD: for whatever pose the game shows **this frame**, draw that
pose's HD frame at the metasprite's on-screen bbox — so the HD animation runs
exactly in sync with gameplay (no free-running loop), driven by the observed
sprite occurrences (each carries `anim_group_id` = the clip, `hash` = the pose,
and its screen bbox).

Tween levels (authoring model section 2.3):
  0 · Pop           — draw the pose's HD frame at the observed bbox (the game
                      already moves the bbox every frame → smooth position).
  1 · Geometric     — interpolate the *transform* (bbox pos/size) between the
                      pose keyframes across the ticks each is held, via the
                      GeometricTween (C-S1, ayther_geo_tween_*), so a single HD
                      per pose glides between keyframes instead of popping.

## Renderer contract

VkSprite today draws a whole texture per AytherSpriteSub. An HD **sheet** (one
image, many pose frames) needs a **sub-rect (UV)** and a float dst rect for
sub-pixel-smooth tweening — this header's `AnimHdFrame`. The renderer-side work
is a `VkSprite::draw_anim(const AnimHdFrame*, n, pack)` variant that samples
the sheet with these UVs (the existing whole-texture path is unchanged).

## Integration status

The source is part of `Ayther::engine`; `AytherSession` resolves frames,
`AytherRenderer` submits the animation lane, and `VkSprite::draw_anim`
samples sheet sub-rectangles. Pack authoring writes animations.toml and the
animation smoke tool exercises the complete path.

This component is a pose-hash-to-frame map, not an independent timeline. The
game supplies the cursor. Level-1 phase state must be reset after detection
loss or non-sequential seeks; callers must not assume backward-seek safety
until that reset contract is enforced.

**Declares:** `AnimationDef`, `AnimationPlayer`, `AnimHdFrame`, `ayther`, `HdPose`, `Impl`

_The installed header (`include/ayther/ayther_animation.h`) carries the full documentation of every symbol._

---

<a id="ayther_audio_eventsh"></a>

## ayther_audio_events.h

ayther_audio_events.h — C-A2: HD substitution of whole audio EVENTS.

## What this is

A per-batch substitution (AudioSubstitutor, ayther_session.h) replaces one
recurring PCM batch hash with an HD asset — right for a short SFX. A sound
with an attack and a tail (a jingle, a voice, music) spans many changing
batches → it needs to be grabbed and replaced as one **event** (a contiguous
run of non-silent batches; AudioEventDetector, core/src/audio_event.rs, keyed
by a stable head `signature`, split on silence or a re-attack — C-A1).

This coordinator turns the detected events + the author's `signature → asset`
assignments into the two per-frame decisions the session's audio needs:

  1. **Range-mute**: while a frame falls inside a substituted event, the
     emulator PCM is suppressed for the WHOLE event — not per hash, because the
     event's batches all have different hashes. The session applies it by
     calling AudioPlayer::discard_emulator() that frame instead of
     flush_emulator() (reusing the existing capability — no player change).

  2. **HD trigger**: at the event's start_frame the HD asset starts playing,
     aligned to the attack (looping until end_frame if the asset loops).

## Session wiring (produce_frame / replay)

  // once, over a take (the Lab authors on a recording; the full timeline is
  // known → events resolve exactly):
  AytherAudioEvent evs[N];
  uint32_t n = ayther_audio_evdet_get_events(detector, evs, N);
  coord.resolve(evs, n);

  // each frame at `frame`:
  if (coord.mute_at(frame)) audio.discard_emulator();
  else                      audio.flush_emulator();
  AudioEventTrigger t[4];
  for (uint32_t i = 0, k = coord.triggers_at(frame, t, 4); i < k; ++i)
      audio.play_event_hd(pack, t[i].asset, t[i].looping);   // OGG/WAV

Live play (ayther_play) uses the same assignments with a short head-latency
variant (a run is recognised once its signature head is complete) — a
follow-up; this contract covers the recording/replay path the Lab authors on.

## Integration status

The detector, substitution FFI, engine coordinator, `AudioPlayer` event
playback, and root target are integrated. Session replay and live paths share
the persisted assignments but retain distinct scheduling policies.

**Declares:** `AudioEventAssignment`, `AudioEventSubstitution`, `AudioEventTrigger`, `ayther`, `Impl`

_The installed header (`include/ayther/ayther_audio_events.h`) carries the full documentation of every symbol._

---

<a id="ayther_core_ffih"></a>

## ayther_core_ffi.h

C declarations for symbols exported from the Rust ayther_core static lib.
Keep this header in sync with core/src/lib.rs.

Most of the type-safe surface now goes through cxx::bridge (core/src/ffi.rs,
integrated via corrosion). The extern-C wrappers below remain ONLY for the
zero-copy hot path (process_frame / update_ram / set_pack — raw pointers cxx
does not bridge); keep them in sync with lib.rs by hand.

`<stdint.h>` instead of `<cstdint>` and a guarded `extern "C"`: a step
towards being includable from C, and from C++ nothing changes.

NOTICE, so as not to half-promise: this header is **not pure C yet**. It uses
`bool` and struct names without a `typedef`, so a `.c` will not compile it.
The SDK's C API is `ayther_sdk.h` —that is the surface designed for C, pack
reading included— and this is a shared contract header in C++.
The `pack_read` example exposed it, by trying to use it from C.

**Declares:** `AudioEventGate`, `AyArchive`, `ayther_apply_rom_patch`, `ayther_asset_id`, `ayther_asset_id_bytes`, `ayther_audio_evdet_event_count`, `ayther_audio_evdet_flush`, `ayther_audio_evdet_free`, `ayther_audio_evdet_get_events`, `ayther_audio_evdet_new`, `ayther_audio_evdet_push`, `ayther_audio_evdet_set_split_on_reattack`, `ayther_audio_event_active`, `ayther_audio_event_clear_events`, `ayther_audio_event_count`, `ayther_audio_event_finish`, `ayther_audio_event_free`, `ayther_audio_event_get`, `ayther_audio_event_new`, `ayther_audio_event_process_frame`, `ayther_audio_event_process_frame_ex`, `ayther_audio_event_reset`, `ayther_audio_event_set_initial_active`, `ayther_audio_event_set_pal`, `ayther_audio_events_format`, `ayther_audio_events_parse`, `ayther_audio_gate_eval`, `ayther_audio_gate_free`, `ayther_audio_gate_new`, `ayther_audio_hasher_end_tick`, `ayther_audio_hasher_free`, `ayther_audio_hasher_get_occurrences`, `ayther_audio_hasher_new`, `ayther_audio_hasher_process_batch`, `ayther_audio_hasher_unique_count`, `ayther_audio_sub_add_event_override`, `ayther_audio_sub_add_override`, `ayther_audio_sub_catalog_len`, `ayther_audio_sub_clear_event_overrides`, `ayther_audio_sub_clear_overrides`, `ayther_audio_sub_event_catalog_len`, `ayther_audio_sub_free`, `ayther_audio_sub_load_pack`, `ayther_audio_sub_new`, `ayther_audio_sub_resolve`, `ayther_audio_sub_resolve_events`, `ayther_bg_stitcher_animated_cells`, `ayther_bg_stitcher_bounds`, `ayther_bg_stitcher_cell_count`, `ayther_bg_stitcher_conflicts`, `ayther_bg_stitcher_free`, `ayther_bg_stitcher_get`, `ayther_bg_stitcher_new`, `ayther_bg_stitcher_observe`, `ayther_chan_bit`, `ayther_chan_index`, `ayther_chip_name`, `ayther_compat_free`, `ayther_compat_grade`, `ayther_compat_json`, `ayther_compat_reason`, `ayther_compat_unverified`, `ayther_compat_unverified_count`, `ayther_core_version`, `ayther_credits_assets`, `ayther_credits_attribution`, `ayther_credits_author`, `ayther_credits_count`, `ayther_credits_free`, `ayther_credits_licenses`, `ayther_credits_role`, `ayther_engine_version`, `ayther_game_profile_assign`, `ayther_game_profile_entities`, `ayther_game_profile_free`, `ayther_game_profile_kind_count`, `ayther_game_profile_kind_name`, `ayther_game_profile_kind_of_id`, `ayther_game_profile_load`, `ayther_game_profile_load_str`, `ayther_geo_tween_duration`, `ayther_geo_tween_free`, `ayther_geo_tween_new`, `ayther_geo_tween_sample`, `ayther_instruments_soundfonts`, `ayther_is_rom_patch`, `ayther_manifest_schema_supported`, `ayther_pack_build_id`, `ayther_pack_builder_add_bytes`, `ayther_pack_builder_add_file`, `ayther_pack_builder_count`, `ayther_pack_builder_finish`, `ayther_pack_builder_free`, `ayther_pack_builder_new`, `ayther_pack_close`, `ayther_pack_compat`, `ayther_pack_credits`, `ayther_pack_declares_systems`, `ayther_pack_default_profile`, `ayther_pack_entry_count`, `ayther_pack_entry_name`, `ayther_pack_entry_streamable`, `ayther_pack_file_size`, `ayther_pack_format_supported`, `ayther_pack_game_id`, `ayther_pack_meta_field`, `ayther_pack_open`, `ayther_pack_open_trusted`, `ayther_pack_profile_count`, `ayther_pack_profile_field`, `ayther_pack_profile_index`, `ayther_pack_profile_muted_buses`, `ayther_pack_profile_systems`, `ayther_pack_read`, `ayther_pack_read_range`, `ayther_pack_report_code`, `ayther_pack_report_count`, `ayther_pack_report_free`, `ayther_pack_report_has_errors`, `ayther_pack_report_message`, `ayther_pack_report_severity`, `ayther_pack_schema`, `ayther_pack_set_region`, `ayther_pack_set_tier`, `ayther_pack_set_tier_for_height`, `ayther_pack_systems`, `ayther_pack_tiers`, `ayther_pack_validate`, `ayther_pack_watcher_free`, `ayther_pack_watcher_new`, `ayther_pack_watcher_poll`, `ayther_palette_signature`, `ayther_pose_sub_add_override`, `ayther_pose_sub_add_override_variants`, `ayther_pose_sub_clear_overrides`, `ayther_pose_sub_free`, `ayther_pose_sub_load_pack`, `ayther_pose_sub_new`, `ayther_pose_sub_resolve`, `ayther_pose_sub_set_cram`, `ayther_pose_sub_set_screen`, `ayther_rom_patch_error`, `ayther_script_free`, `ayther_script_get_audio_overrides`, `ayther_script_get_shader_params`, `ayther_script_get_sprite_overrides`, `ayther_script_get_tile_overrides`, `ayther_script_load_string`, `ayther_script_new`, `ayther_script_on_frame`, `ayther_script_set_pack`, `ayther_script_update_audio`, `ayther_script_update_sprites`, `ayther_script_update_tiles`, `ayther_scroll_unwrapper_free`, `ayther_scroll_unwrapper_last_step`, `ayther_scroll_unwrapper_new`, `ayther_scroll_unwrapper_push`, `ayther_sf2_all_notes_off`, `ayther_sf2_bake`, `ayther_sf2_control`, `ayther_sf2_free`, `ayther_sf2_list_presets`, `ayther_sf2_new`, `ayther_sf2_new_shared`, `ayther_sf2_note_off`, `ayther_sf2_note_on`, `ayther_sf2_preset_list`, `ayther_sf2_program`, `ayther_sf2_render`, `ayther_sf2_trim_cache`, `ayther_sonic_read_velocity`, `ayther_sonic_read_xy`, `ayther_soundfont_normalize_file`, `ayther_sprite_hasher_clip_count`, `ayther_sprite_hasher_free`, `ayther_sprite_hasher_get_clip`, `ayther_sprite_hasher_get_occurrences`, `ayther_sprite_hasher_new`, `ayther_sprite_hasher_process_sprites`, `ayther_sprite_hasher_process_vram`, `ayther_sprite_hasher_reset_animation_grouper`, `ayther_sprite_hasher_unique_count`, `ayther_sprite_sub_add_override`, `ayther_sprite_sub_add_override_ref`, `ayther_sprite_sub_clear_overrides`, `ayther_sprite_sub_free`, `ayther_sprite_sub_load_pack`, `ayther_sprite_sub_new`, `ayther_sprite_sub_resolve`, `ayther_subsystem_count`, `ayther_subsystem_name`, `ayther_tile_brightness_factor`, `ayther_tile_hasher_dump_toml`, `ayther_tile_hasher_free`, `ayther_tile_hasher_get_occurrences`, `ayther_tile_hasher_new`, `ayther_tile_hasher_process_frame`, `ayther_tile_hasher_unique_count`, `ayther_tile_mean_level`, `ayther_tile_shape_hash`, `ayther_tile_sub_add_override`, `ayther_tile_sub_begin_frame`, `ayther_tile_sub_clear_overrides`, `ayther_tile_sub_free`, `ayther_tile_sub_load_pack`, `ayther_tile_sub_load_pack_named`, `ayther_tile_sub_lookup`, `ayther_tile_sub_new`, `ayther_tile_sub_resolve`, `ayther_tween_begin_frame`, `ayther_tween_clear`, `ayther_tween_clear_overrides`, `ayther_tween_free`, `ayther_tween_load_pack`, `ayther_tween_new`, `ayther_tween_resolve`, `ayther_tween_set_override`, `ayther_widescreen_gate_eval`, `ayther_widescreen_gate_free`, `ayther_widescreen_gate_new`, `AytherAnimFrame`, `AytherAudioActive`, `AytherAudioActiveSub`, `AytherAudioEvent`, `AytherAudioEventDetector`, `AytherAudioEventSub`, `AytherAudioHasher`, `AytherAudioOccurrence`, `AytherAudioOverride`, `AytherAudioSub`, `AytherAudioSubstitutor`, `AytherAudioWrite`, `AytherBatchEventDetector`, `AytherBgStitcher`, `AytherCompat`, `AytherCredits`, `AytherEventSub`, `AytherGameProfile`, `AytherGeometricTween`, `AytherPackBuilder`, `AytherPackReport`, `AytherPackWatcher`, `AytherPcmEvent`, `AytherScriptEnv`, `AytherScrollUnwrapper`, `AytherSf2`, `AytherShaderParams`, `AytherSpriteHasher`, `AytherSpriteOccurrence`, `AytherSpriteOverride`, `AytherSpriteSub`, `AytherSpriteSubstitutor`, `AytherTileHasher`, `AytherTileOccurrence`, `AytherTileOverride`, `AytherTileSub`, `AytherTileSubstitutor`, `AytherTransform`, `PoseSetSubstitutor`, `struct`, `TweenPlayer`, `WidescreenGate`

_The installed header (`include/ayther/ayther_core_ffi.h`) carries the full documentation of every symbol._

---

<a id="ayther_layersh"></a>

## ayther_layers.h

AytherLayerStack is the engine's first-class layer model.

The stack combines the VDP planes and renderer lanes into one explicit draw
order. Each layer has independent visibility, and custom layers can be
inserted at any position to support effects such as parallax.

Element-level visibility is deliberately not represented here. It belongs
to the session inventory and is controlled through
AytherSession::set_hidden_elements().

**Declares:** `AytherLayer`, `AytherLayerContent`, `AytherLayerStack`, `uint8_t`

_The installed header (`include/ayther/ayther_layers.h`) carries the full documentation of every symbol._

---

<a id="ayther_mode3h"></a>

## ayther_mode3.h

ayther_mode3.h — Mode 3 (RAM anchoring) resolver: metasprite substitution
*per entity instance**.

## What Mode 3 is

Mode 2 (define_metasprite, ayther_session.h) groups SAT sprites by
anim_group_id and substitutes the whole bounding box with one HD asset. It
cannot tell two *identical* on-screen entities apart (two of the same enemy,
Sonic + Tails): their sprites share hashes/anim-groups, so a purely visual
split guesses. Mode 3 removes the guess by reading each entity's **world
position** from game RAM — every instance has a distinct world_pos, so the SAT
sprites split exactly, and each instance gets its HD asset placed at its own
bbox.

## How it plugs in

  AytherSession::produce_frame(), once per frame:
    1. reads the foreground plane camera scroll from the VDP (already computed
       for the Fase 2c plane resolver): Hscroll table (VRAM) + VSRAM.
    2. calls Mode3Resolver::resolve(work_ram, scroll, plane size, sprite_occs).
    3. publishes resolver.subs() on the FrameView (a per-instance HD lane), and
       resolver.instances() for authoring overlays (the Lab draws each anchored
       entity's box + id).

The resolver owns the Rust game profile (game_profile.rs, via the
ayther_game_profile_* FFI) and does the geometry through
ayther_game_profile_assign — the exact path validated against Sonic 2 EHZ in
tools/mode3_spike (VDP scroll read == RAM camera; Sonic/Tails split by
world_pos).

## Integration status

The Rust profile implementation, FFI, engine resolver, session wiring, and
root CMake target are integrated. The design mirrors the existing
SpriteSubstitutor/metasprite flow and publishes results through FrameView.

**Declares:** `ayther`, `EntityInstance`, `Impl`, `Mode3Resolver`

_The installed header (`include/ayther/ayther_mode3.h`) carries the full documentation of every symbol._

---

<a id="ayther_resulth"></a>

## ayther_result.h

ayther_result.h — no-throw error model for the runtime / FFI boundary.

The FFI never propagates exceptions or panics across the binary boundary
(see docs/API_COMPATIBILITY.md#error-handling). Expected failures (corrupt
pack, malformed TOML, missing ROM) surface as an ayther::Result, so the
caller — especially ayther_lab — can show *why* something failed, not just
that* it failed.

C++20: no std::expected (that is C++23). This is the project's vehicle.

**Declares:** `ayther`, `Error`, `ErrorCode`, `Result`

_The installed header (`include/ayther/ayther_result.h`) carries the full documentation of every symbol._

---

<a id="ayther_sdk_versionh"></a>

## ayther_sdk_version.h

ayther_sdk_version.h — SDK version and compatibility contract.

AYTHER has independent core-ABI, emulator-extension, and pack-format version
axes. This header defines the native SDK version that installed consumers
compile against.

SemVer rule for the current 0.x series: a minor-version change may break the
contract. The CMake package enforces the same rule with SameMinorVersion.

Two values must remain distinguishable:

  - compile-time: `AYTHER_SDK_VERSION_*` from the installed headers;
  - link/run-time: `ayther::sdk_version()` from the linked library.

They can differ when a program finds an unexpected library at run time.
`sdk_version_check()` reports both values so incompatibility fails with a
diagnostic rather than at the first changed layout or call contract.

**Declares:** `ayther`, `SdkVersion`

_The installed header (`include/ayther/ayther_sdk_version.h`) carries the full documentation of every symbol._

---

<a id="ayther_sdkh"></a>

## ayther_sdk.h

**Declares:** `ay_add_audio_observer`, `ay_add_frame_observer`, `ay_add_post_filter`, `ay_audio_events`, `ay_capabilities`, `ay_clear_pack`, `ay_compat_close`, `ay_compat_grade`, `ay_compat_json`, `ay_compat_reason`, `ay_compat_runnable`, `ay_compat_unverified`, `ay_compat_unverified_count`, `ay_core_option_count`, `ay_core_option_desc`, `ay_core_option_key`, `ay_create`, `ay_destroy`, `ay_error_message`, `ay_export_frame`, `ay_export_frame_size`, `ay_extension_active`, `ay_extension_failures`, `ay_frame`, `ay_game_id`, `ay_get_input`, `ay_has_pack`, `ay_last_create_error`, `ay_memory_size`, `ay_pack_close`, `ay_pack_compat`, `ay_pack_entry_count`, `ay_pack_entry_name`, `ay_pack_entry_size`, `ay_pack_entry_streamable`, `ay_pack_game_id`, `ay_pack_open`, `ay_pack_read_entry`, `ay_pack_read_range`, `ay_read_memory`, `ay_remove_extension`, `ay_set_input`, `ay_set_pack`, `ay_step`, `AyAudioEvent`, `AyButton`, `AyCapability`, `AyCompat`, `AyFrame`, `AyPack`, `AyPixelFormat`, `AySession`, `AySessionConfig`, `AyStatus`, `enum`, `int`, `struct`, `void`

_The installed header (`include/ayther/ayther_sdk.h`) carries the full documentation of every symbol._

---

<a id="ayther_sessionh"></a>

## ayther_session.h

ayther_session.h — AytherSession, the motor's control facade (R2).

AytherSession is the single, coherent surface the frontends drive instead of
wiring the ~9 raw ayther_core handles by hand. It owns the whole deterministic
pipeline — emulator host + tile/sprite/audio hashing + substitution + Lua
scripting + HD audio output — behind one object, and exposes the *result* of
each frame as a plain-data FrameView for the frontend to render.

Engine / frontend boundary (see docs/ARCHITECTURE.md#session-and-renderer-boundary):

  AytherSession (motor, this object)        Frontend (ayther_play / ayther_lab)
  ----------------------------------        -----------------------------------
  run_frame() the libretro core             window + SDL events + input source
  hash tiles/sprites/audio                   upload FrameView.fb -> VkTexture
  fire Lua on_frame, resolve overrides       draw tile/sprite substitutions
  resolve substitutions                      CRT post-process / present
  play + mute + flush HD audio   <-- audio   Lab authoring UI / timeline
  produce a FrameView  ----------------->    consume FrameView, render it

Audio output lives entirely inside the session (the AudioPlayer is owned by
the motor in R1): play/mute/flush happen in step(), nothing audio crosses the
boundary. The frontend never touches Vulkan-from-the-motor or SDL-audio.

No-throw: construction and fallible operations return ayther::Result (§4.1.1);
nothing throws across the FFI users. Opaque handles are held as
ayther::unique_handle inside the pimpl — no raw Rust pointer lives loose (§4.1).

Threading: a session is single-owner and must be driven from one thread (the
emulation thread), matching ayther_core's rule. Non-copyable; movable.

**Declares:** `ayther`, `ayther_plane_tile_hash_repalette`, `ayther_plane_tile_hash_variants`, `AytherRecording`, `AytherSession`, `ElementEffect`, `EnhancedElement`, `FrameView`, `HiddenElement`, `PlaneCellHit`, `PlaneTileOccurrence`, `SceneElement`, `subsystem_bit`, `uint8_t`

_The installed header (`include/ayther/ayther_session.h`) carries the full documentation of every symbol._

---

<a id="ayther_versionh"></a>

## ayther_version.h

Canonical AYTHER release and compatibility version contract.

AYTHER_VERSION_* identifies the product release shared by Cargo, CMake, the
native SDK, the engine compatibility check, and the Lua API. The remaining
values are independent protocol revisions and do not follow SemVer.

_The installed header (`include/ayther/ayther_version.h`) carries the full documentation of every symbol._

---

<a id="enginecapabilitieshpp"></a>

## engine/capabilities.hpp

**Declares:** `Capabilities`, `Version`

_The installed header (`include/ayther/engine/capabilities.hpp`) carries the full documentation of every symbol._

---

<a id="engineenginehpp"></a>

## engine/engine.hpp

_The installed header (`include/ayther/engine/engine.hpp`) carries the full documentation of every symbol._

---

<a id="logh"></a>

## log.h

log.h — the engine's only way to say something.

The engine used to call fprintf(stderr, "[Component] ...") from ~200 places.
That is invisible to a frontend: a GUI cannot show it, a test cannot assert
on it, and a host embedding the engine cannot route it anywhere. Worse, the
hot paths measured 5 ms per line against a Windows console, which is why
several call sites grew ad-hoc "log once" flags.

Every record now carries four things a consumer can act on -- severity, the
component that spoke, a STABLE event id, and typed fields -- plus the human
message. A frontend installs a sink and decides what happens. When nobody
installs one, the built-in fallback writes to stderr; that fallback is the
single place in the engine allowed to touch a console stream.

Engine-internal header: not installed, so it must not appear in any public
header.

**Declares:** `emit`, `Field`, `Record`, `set_min_severity`, `set_sink`, `uint8_t`, `Value`, `write`

_The installed header (`include/ayther/log.h`) carries the full documentation of every symbol._
