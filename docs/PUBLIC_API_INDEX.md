# AYTHER Engine — index of installed headers

> **GENERATED — do not edit by hand.** `pwsh tools/gen_api_reference.ps1`.
> Derived from `include/ayther/**/*.h`, the same root that
> `cmake/AytherInstall.cmake` installs: if a header enters
> or leaves the surface, this page reflects it without anyone editing a
> parallel list.

The installed surface and its stability are described in
[`API_COMPATIBILITY.md`](API_COMPATIBILITY.md).
Appearing in this index does not by itself imply a stability guarantee.

## The 46 headers

| header | what it provides |
|---|---|
| [`audio_asset_level.h`](#audio_asset_levelh) | Installed public header. |
| [`audio_bus_balance.h`](#audio_bus_balanceh) | Installed public header. |
| [`audio_hd_mixer.h`](#audio_hd_mixerh) | Installed public header. |
| [`audio_live_resume.h`](#audio_live_resumeh) | Installed public header. |
| [`audio_match_rule.h`](#audio_match_ruleh) | Installed public header. |
| [`audio_player.h`](#audio_playerh) | Installed public header. |
| [`audio_seq_anchor.h`](#audio_seq_anchorh) | Installed public header. |
| [`ayther_animation.h`](#ayther_animationh) | Installed public header. |
| [`ayther_audio_events.h`](#ayther_audio_eventsh) | Installed public header. |
| [`ayther_background_export.h`](#ayther_background_exporth) | Installed public header. |
| [`ayther_components_toml.h`](#ayther_components_tomlh) | Installed public header. |
| [`ayther_config.h`](#ayther_configh) | Installed public header. |
| [`ayther_core_ffi.h`](#ayther_core_ffih) | Installed public header. |
| [`ayther_env.h`](#ayther_envh) | Installed public header. |
| [`ayther_layers.h`](#ayther_layersh) | Installed public header. |
| [`ayther_mode3.h`](#ayther_mode3h) | Installed public header. |
| [`ayther_rank.h`](#ayther_rankh) | Installed public header. |
| [`ayther_recording.h`](#ayther_recordingh) | Installed public header. |
| [`ayther_renderer.h`](#ayther_rendererh) | Installed public header. |
| [`ayther_result.h`](#ayther_resulth) | Installed public header. |
| [`ayther_sdk_version.h`](#ayther_sdk_versionh) | Installed public header. |
| [`ayther_sdk.h`](#ayther_sdkh) | Installed public header. |
| [`ayther_session.h`](#ayther_sessionh) | Installed public header. |
| [`ayther_unique_handle.h`](#ayther_unique_handleh) | Installed public header. |
| [`ayther_version.h`](#ayther_versionh) | Installed public header. |
| [`ayther_video.h`](#ayther_videoh) | Installed public header. |
| [`cram_palette.h`](#cram_paletteh) | Installed public header. |
| [`failure_escalation.h`](#failure_escalationh) | Installed public header. |
| [`libretro_host/ayther_api.h`](#libretro_hostayther_apih) | Installed public header. |
| [`libretro_host/core_loader.h`](#libretro_hostcore_loaderh) | Installed public header. |
| [`libretro_host/libretro.h`](#libretro_hostlibretroh) | Installed public header. |
| [`libretro_host/retro_runner.h`](#libretro_hostretro_runnerh) | Installed public header. |
| [`output_profile.h`](#output_profileh) | Installed public header. |
| [`pano_bands.h`](#pano_bandsh) | Installed public header. |
| [`panorama_cover.h`](#panorama_coverh) | Installed public header. |
| [`parallax_bands.h`](#parallax_bandsh) | Installed public header. |
| [`psg_synth.h`](#psg_synthh) | Installed public header. |
| [`rewind_buffer.h`](#rewind_bufferh) | Installed public header. |
| [`voice_router.h`](#voice_routerh) | Installed public header. |
| [`vulkan_backend/tile_tex_cache.h`](#vulkan_backendtile_tex_cacheh) | Installed public header. |
| [`vulkan_backend/vk_context.h`](#vulkan_backendvk_contexth) | Installed public header. |
| [`vulkan_backend/vk_indexed_plane.h`](#vulkan_backendvk_indexed_planeh) | Installed public header. |
| [`vulkan_backend/vk_render_target.h`](#vulkan_backendvk_render_targeth) | Installed public header. |
| [`vulkan_backend/vk_sprite.h`](#vulkan_backendvk_spriteh) | Installed public header. |
| [`vulkan_backend/vk_texture.h`](#vulkan_backendvk_textureh) | Installed public header. |
| [`widescreen.h`](#widescreenh) | Installed public header. |

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

<a id="audio_bus_balanceh"></a>

## audio_bus_balance.h

audio_bus_balance.h — BETWEEN-BUS normalization (second half).

The first half—measuring each asset (peak, RMS, clipping, and suggested
correction)—already lives in `audio_asset_level.h` and is visible in Mix.
It lets the author fix an asset that clips or gets masked. It does not fix
the problem that appears only once the pack is complete: **music and sound
effects can each be correct on their own yet clash with each other**. A pack
where every hit masks the music may still contain flawless individual assets.

Why this is a separate calculation rather than "the same one, averaged":

  · It averages ENERGY, not decibels. Averaging -6 dB and -30 dB gives
    -18 dB, which is not the perceived level because -6 dB dominates.
    Averaging energy first and then converting to dB yields a value that
    corresponds to what is actually heard.
  · It is weighted by DURATION. A three-minute track and a 200 ms hit do not
    contribute equally to the perceived bus volume; counting them equally
    would make a pack with many short effects appear loud.
  · It includes the ALREADY-AUTHORED gain. The author may have lowered an
    asset manually; balance must consider what will be heard, not merely
    what the file contains.

MUSIC IS THE REFERENCE. It is continuous material: the ear uses it to set
the scene level, and it keeps playing when nothing else happens. Without
classified music, the reference is the bus with the most measured material;
if there is only one bus, there is nothing to balance and that is reported.

What this calculation does NOT do: modify the file or the per-asset gain.
Its output is a PER-BUS correction, exactly what
`AytherSession::set_bus_volume` applies live and the project persists.
Correcting the bus rather than each asset also reflects the measurement
honestly: the bus is what was found to be unbalanced.

This is stateless and defined in the public header—with its oracle,
`audio_bus_balance`—because the rule belongs to the CALCULATION and can
therefore be tested without an audio engine.

**Declares:** `audio_bus_balance`, `AudioBusBalance`, `AudioBusLevel`, `AudioBusSample`, `ay_db_to_lin`, `ay_lin_to_db`, `ayther`

_The installed header (`include/ayther/audio_bus_balance.h`) carries the full documentation of every symbol._

---

<a id="audio_hd_mixerh"></a>

## audio_hd_mixer.h

audio_hd_mixer.h — HD voice mixer on the main stream's SAMPLE timeline.

THE PROBLEM IT SOLVES. HD replacements used to run in their own SDL streams:
they started "now" according to wall-clock time while the original traveled
through `emu_stream_` with a ~70 ms DRC cushion. The phase between original
and HD therefore depended on backlog, stalls, and catch-up size. Here every
voice is PLACED at an absolute sample on the staged block timeline and mixed
INSIDE that block: a trigger at frame N lands on the same sample under 1x1
execution or catch-up 16, and everything—original, router, and HD—crosses
the SAME DRC/backlog. Pausing one stream pauses everything.

WHAT THIS MODULE IS. Mixing only: voices with already decoded and converted
PCM (S16 stereo at 44100 Hz, guaranteed by the AudioPlayer cache), sample
placement, phase-preserving loops, gain, cut fades, and the per-frame
lifetime contract (end + tail). It does NOT touch SDL: mixing is a pure
function over a buffer, so the 1x1-vs-catch-up identity oracle can be exact,
byte for byte, without a device.

The FRAME-based lifecycle (`end_frame`/`cut_frame`) deliberately remains in
frames: it is the same contract used by `tick_events` and session windows.
Frame-to-sample conversion lives in ONE place (start placement), rather than
being scattered across every sweep.

**Declares:** `HdMixer`, `voices_`

_The installed header (`include/ayther/audio_hd_mixer.h`) carries the full documentation of every symbol._

---

<a id="audio_live_resumeh"></a>

## audio_live_resume.h

audio_live_resume.h — PURE resume decision for a live replacement.

A pause physically stops the HD streams, but the LOGICAL replacement
instance—which asset, anchored to which frame, and until when—remains alive
in the session. On resume, playback must continue FROM THE OFFSET dictated
by the emulated clock, not from zero: clearing edges and retriggering removes
the silence but introduces drift, which is explicitly forbidden.

The chosen strategy is to RECREATE the stream at the offset rather than
freeze the physical stream. It uses the same arithmetic as the take path
(`(f - anchor) / fps`), survives Assets OFF/ON and workspace changes through
the same code, and is compatible with a future unified mixer: the logical
instance knows nothing about SDL.

This header is PURE (no SDL and no core), so the decision can be tested
without a session or ROM, following the same criterion as transport_gate.h.

**Declares:** `ayther`, `live_instance_over`, `live_resume_decide`, `live_resume_offset_bytes`, `LiveResumeDecision`, `uint8_t`

_The installed header (`include/ayther/audio_live_resume.h`) carries the full documentation of every symbol._

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

<a id="audio_playerh"></a>

## audio_player.h

audio_player.h — SDL3 audio device: emulator passthrough + HD WAV playback.

PCM passthrough via continuous emu_stream_ + one-shot HD SFX streams.
v0.9.1:  Mute-on-substitution — emulator PCM is suppressed for any hash that
         had a resolved HD substitution in the previous tick.  This left a
         1-tick (~16 ms) bleed on the first appearance of a new substitution,
         because the audio callback fires *during* run_frame(), before the
         substitution for the current frame has been resolved.
v0.9.7:  Deferred passthrough — the audio callback no longer pushes PCM
         directly.  It buffers each batch (tagged with its hash) via
         buffer_emulator(); the main loop resolves substitutions for the
same* frame, refreshes the mute set, then calls flush_emulator(),
         which pushes only the non-muted batches.  The mute decision now uses
         the current frame's resolution, eliminating the 1-tick bleed.

Design:
  emu_stream_     Continuous emulator PCM passthrough (S16 stereo, 44100 Hz).

  pending_pcm_    Per-frame interleaved PCM staged by buffer_emulator() and
  pending_batches_  drained by flush_emulator().  Capacity is reused across
                  frames (no steady-state allocation).

  sfx_streams_    One-shot streams for HD WAV substitutions.
                  Created in play_substitutions(), reaped in tick() once drained.

  mute_hashes_    Set of hashes whose emulator PCM should be suppressed this
                  frame.  Refreshed via set_mute_hashes() from resolved subs.

  wav_cache_      WAV assets decoded once per asset_path and held in memory.
                  Freed in shutdown().

Thread model: all public methods are called from the main/emulation thread.
SDL3 audio streams are internally thread-safe for the push side.

Typical per-tick call sequence:

  // inside runner audio callback (called during run_frame()):
  uint64_t hash = ayther_audio_hasher_process_batch(hasher, data, frames);
  audio_player.buffer_emulator(hash, data, frames);

  // after ayther_audio_sub_resolve() — unconditional, clears set when 0:
  audio_player.play_substitutions(pack, audio_subs, n_audio_subs);
  audio_player.set_mute_hashes(audio_subs, n_audio_subs);
  audio_player.flush_emulator();   // pushes non-muted batches to emu_stream_

  // end of frame:
  audio_player.tick();

**Declares:** `AudioPlayer`

_The installed header (`include/ayther/audio_player.h`) carries the full documentation of every symbol._

---

<a id="audio_seq_anchorh"></a>

## audio_seq_anchor.h

audio_seq_anchor.h — replay Sequence anchors with CLAIMS between Sequences.
Header-only and pure: testable without SDL, a core, or a GPU.

A Sequence (substitution) opens a window at every occurrence of its TRIGGER
signature among the events detected in the take. Rules:

 1. Greedy segmentation (2026-07-23 report): the step is the event SPAN. A
    trigger occurrence inside the previous window's step is INTERNAL (the
    melody repeats its first note) and does not re-anchor. A REAL repetition
    after the step does re-anchor and retrigger.

 2. CLAIM (2026-08-21 report): an occurrence of S's trigger inside ANOTHER
    Sequence T's window (with HD), where T contains that signature as a
    MEMBER, belongs to T. S neither anchors nor triggers: "the one already
    playing wins." In Golden Axe, "The Battle - Intro" and "- Loop" share
    26 signatures; the hi-hat that opens the Intro reappears every 63 frames
    inside the Loop, while the bass that opens the Loop appears in the Intro,
    causing both to play at once.

 3. HEAD (2026-08-21 report, second pass): a lone trigger is fragile. On the
    third Loop pass, the opening bass uses ANOTHER signature (a variant),
    while the other five channels start identically. Without the trigger,
    the Loop failed to re-anchor, its window expired, and the Intro leaked in
    (intro, loop, intro, loop...). The head is the set of signatures that
    start on the SAME frame as the trigger. A Sequence also anchors when a
    MAJORITY of its head starts (>= ceil(n/2), with n >= 2), even without the
    trigger.

 4. Frame tie (two Sequences start on the SAME frame): CONTINUATION wins
    first—a looping Sequence whose window expires on that frame continues
    ("always keep the one already playing unless it ended or the events
    changed"). Next comes the most SPECIFIC Sequence (fewest member
    signatures), then ID as a deterministic tie-breaker.

Events are traversed in ascending `start_frame` order. The detector does NOT
return them sorted (it emits them per channel), so the table applies a stable
sort. The same traversal drives playback, bare-frame muting, and export
mixdown: ONE table and one policy.

**Declares:** `ayther`, `seq_anchor_frame`, `seq_head_quorum`, `seq_sub_before`, `seq_sub_claims`, `SeqAnchorState`, `SeqAnchorSub`

_The installed header (`include/ayther/audio_seq_anchor.h`) carries the full documentation of every symbol._

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

<a id="ayther_background_exporth"></a>

## ayther_background_export.h

ayther_background_export.h — Fondos: reconstruct a plane's level strip into a
*per-layer PNG** the artist repaints in HD (componentes-plan §3, the
first-order deliverable).

## What this is

`BackgroundStitcher` (core/src/background.rs) accumulates a scrolling plane's
visible cells into a full level strip in level space over a `.arp` take — no
single frame holds a level, since the nametable wraps. This exporter turns
that accumulated strip into a dense RGBA image and writes it as a PNG whose
name carries the **index** (plane + level origin + a layout hash), so when the
artist brings the repainted HD back it maps unambiguously to its plane and
position: `bg_A_t000x000_<hash>.png`.

The pixels come from decoding each cell's pattern (via a `TileDecodeFn` — the
engine passes AytherSession::decode_plane_tile). Cells the stitcher flagged as
*animated** are still written (as their representative frame) and reported in
`LayerImage::animated`, so the authoring UI can mark them.

## STATUS — contract/skeleton (env-blocked)

The core stitcher + FFI are done and validated (tools/background_spike: 167-tile
strip, 0 conflicts). This header + ayther_background_export.cpp are the
engine-side consumer, ready to wire once the engine builds (needs vcpkg for
stb_image_write). ayther_background_export.cpp is NOT yet in
the root CMakeLists.txt — it type-checks standalone (see its header comment).

**Declares:** `ayther`, `BackgroundExporter`, `LayerImage`

_The installed header (`include/ayther/ayther_background_export.h`) carries the full documentation of every symbol._

---

<a id="ayther_components_tomlh"></a>

## ayther_components_toml.h

ayther_components_toml.h — TOML round-trip of the Components layer:
`animations.toml` (C-S4) and `audio_events.toml` (C-A4).

Baking (bake_*) is called by the Lab's Deliver step while building the pack;
parsing (parse_*) is called by AytherSession when loading a pack
(load_pack_into), repopulating the AnimationPlayer / the per-event assignment
mirror. Both sides live in the ENGINE (free functions, no UI and no Vulkan)
so the round-trip is testable headless with strings.

Formats:
  animations.toml (engine-owned):
    [[animation]]
    clip  = "0x<16hex>"          # authoring handle (clip id)
    sheet = "sheets/run.png"
    tween = 1                     # 0 Pop · 1 geometric tween
    [[animation.pose]]
    pose   = "0x<16hex>"          # pose hash (stable identity)
    src    = [x, y, w, h]         # sub-rect of the sheet (px)
    anchor = [x, y, w, h]         # keyframe dst (Level 1)
    ticks  = 6

  audio_events.toml — the SAME schema the Rust core parses
  (AudioSubstitutor::parse_events_toml, loaded by load_from_pack):
    [[event]]
    signature = "0x<16hex>"
    asset     = "audio/music/zone1.ogg"
    loop      = true              # optional (default false)

  plane_sets.toml — Props (CU002) and Glyphs (CU005): HD substitution per
  multi-tile plane ELEMENT. Until now the Paint catalogue existed only in the
  authoring session (injected through the API), so the delivered `.ay` did
  NOT reproduce any multi-tile substitution; this file closes that gap.
    [[font]]
    id = "0x<16hex>" · name = "HUD" · cell_w = 1 · cell_h = 2
    [[set]]
    id      = "0x<16hex>"        # pintar_element_id (deterministic per capture)
    name    = "Chest"            # informational (overlay/debug)
    type    = "utileria"         # utileria | glifo
    plane   = 0                   # 0=A · 1=B · 2=Window
    w_cells = 3 · h_cells = 2     # bbox
    asset   = "cofre.png"         # basename (the bake routes it to the tier)
    tiles   = "0x<hash>:cx,cy|…"  # members with a RELATIVE offset in cells
    font    = "0x<16hex>" · ch = "A"    # type="glifo" only

  The FLIPS observed at capture time are deliberately NOT baked: the plane
  tile hash is flip-invariant and the matcher does not require them (a
  mirrored prop matches all the same). They live only in
  pintar_elements.toml, which uses them for the faithful export of the base
  PNG.

**Declares:** `ayther`, `bake_animations_toml`, `bake_audio_events_toml`, `bake_elements_toml`, `bake_enhance_toml`, `bake_kinematics_toml`, `bake_panoramas_toml`, `bake_plane_sequences_toml`, `bake_plane_sets_toml`, `bake_screens_toml`, `PackEnhance`, `PackInstrument`, `PackKinematic`, `PackKinematicStep`, `PackPanorama`, `PackPanoramaCell`, `PackPlaneFont`, `PackPlaneSeqStep`, `PackPlaneSequence`, `PackPlaneSet`, `PackPlaneSetMember`, `PackScreen`, `PackScreenCell`, `parse_animations_toml`, `parse_audio_events_toml`, `parse_elements_toml`, `parse_enhance_toml`, `parse_instruments_toml`, `parse_kinematics_toml`, `parse_panoramas_toml`, `parse_plane_sequences_toml`, `parse_plane_sets_toml`, `parse_screens_toml`, `plane_sequence_step_at`, `plane_sequence_total`

_The installed header (`include/ayther/ayther_components_toml.h`) carries the full documentation of every symbol._

---

<a id="ayther_configh"></a>

## ayther_config.h

ayther_config.h — persistent user configuration for Ayther Engine + Lab.

Storage: %APPDATA%\Ayther\config.toml  (Windows)
         ~/.config/Ayther/config.toml   (Linux / macOS — future)

Created automatically on first run with sensible defaults.
The same config instance is shared between the engine (launcher, ROM paths)
and the Lab (projects directory).  Pass a pointer via ILabPlugin::set_config.

Call sequence:
  AytherConfig config = AytherConfig::load();
  // ... use config.rom_library etc. ...
  config.push_recent(rom_path);
  config.save();

**Declares:** `AytherConfig`

_The installed header (`include/ayther/ayther_config.h`) carries the full documentation of every symbol._

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

**Declares:** `AudioEventGate`, `AyArchive`, `ayther_apply_rom_patch`, `ayther_asset_id`, `ayther_asset_id_bytes`, `ayther_audio_evdet_event_count`, `ayther_audio_evdet_flush`, `ayther_audio_evdet_free`, `ayther_audio_evdet_get_events`, `ayther_audio_evdet_new`, `ayther_audio_evdet_push`, `ayther_audio_evdet_set_split_on_reattack`, `ayther_audio_event_active`, `ayther_audio_event_clear_events`, `ayther_audio_event_count`, `ayther_audio_event_finish`, `ayther_audio_event_free`, `ayther_audio_event_get`, `ayther_audio_event_new`, `ayther_audio_event_process_frame`, `ayther_audio_event_process_frame_ex`, `ayther_audio_event_reset`, `ayther_audio_event_set_initial_active`, `ayther_audio_event_set_pal`, `ayther_audio_events_format`, `ayther_audio_events_parse`, `ayther_audio_gate_eval`, `ayther_audio_gate_free`, `ayther_audio_gate_new`, `ayther_audio_hasher_end_tick`, `ayther_audio_hasher_free`, `ayther_audio_hasher_get_occurrences`, `ayther_audio_hasher_new`, `ayther_audio_hasher_process_batch`, `ayther_audio_hasher_unique_count`, `ayther_audio_sub_add_event_override`, `ayther_audio_sub_add_override`, `ayther_audio_sub_catalog_len`, `ayther_audio_sub_clear_event_overrides`, `ayther_audio_sub_clear_overrides`, `ayther_audio_sub_event_catalog_len`, `ayther_audio_sub_free`, `ayther_audio_sub_load_pack`, `ayther_audio_sub_new`, `ayther_audio_sub_resolve`, `ayther_audio_sub_resolve_events`, `ayther_bg_stitcher_animated_cells`, `ayther_bg_stitcher_bounds`, `ayther_bg_stitcher_cell_count`, `ayther_bg_stitcher_conflicts`, `ayther_bg_stitcher_free`, `ayther_bg_stitcher_get`, `ayther_bg_stitcher_new`, `ayther_bg_stitcher_observe`, `ayther_chan_bit`, `ayther_chan_index`, `ayther_chip_name`, `ayther_compat_free`, `ayther_compat_grade`, `ayther_compat_json`, `ayther_compat_reason`, `ayther_compat_unverified`, `ayther_compat_unverified_count`, `ayther_core_version`, `ayther_credits_assets`, `ayther_credits_attribution`, `ayther_credits_author`, `ayther_credits_count`, `ayther_credits_free`, `ayther_credits_licenses`, `ayther_credits_role`, `ayther_engine_version`, `ayther_game_profile_assign`, `ayther_game_profile_entities`, `ayther_game_profile_free`, `ayther_game_profile_kind_count`, `ayther_game_profile_kind_name`, `ayther_game_profile_kind_of_id`, `ayther_game_profile_load`, `ayther_game_profile_load_str`, `ayther_geo_tween_duration`, `ayther_geo_tween_free`, `ayther_geo_tween_new`, `ayther_geo_tween_sample`, `ayther_instruments_soundfonts`, `ayther_is_rom_patch`, `ayther_manifest_schema_supported`, `ayther_pack_build_id`, `ayther_pack_builder_add_bytes`, `ayther_pack_builder_add_file`, `ayther_pack_builder_count`, `ayther_pack_builder_finish`, `ayther_pack_builder_free`, `ayther_pack_builder_new`, `ayther_pack_close`, `ayther_pack_compat`, `ayther_pack_credits`, `ayther_pack_declares_systems`, `ayther_pack_default_profile`, `ayther_pack_entry_count`, `ayther_pack_entry_name`, `ayther_pack_entry_streamable`, `ayther_pack_file_size`, `ayther_pack_format_supported`, `ayther_pack_game_id`, `ayther_pack_meta_field`, `ayther_pack_open`, `ayther_pack_profile_count`, `ayther_pack_profile_field`, `ayther_pack_profile_index`, `ayther_pack_profile_muted_buses`, `ayther_pack_profile_systems`, `ayther_pack_read`, `ayther_pack_read_range`, `ayther_pack_report_code`, `ayther_pack_report_count`, `ayther_pack_report_free`, `ayther_pack_report_has_errors`, `ayther_pack_report_message`, `ayther_pack_report_severity`, `ayther_pack_schema`, `ayther_pack_set_region`, `ayther_pack_set_tier`, `ayther_pack_set_tier_for_height`, `ayther_pack_systems`, `ayther_pack_tiers`, `ayther_pack_validate`, `ayther_pack_watcher_free`, `ayther_pack_watcher_new`, `ayther_pack_watcher_poll`, `ayther_palette_signature`, `ayther_pose_sub_add_override`, `ayther_pose_sub_add_override_variants`, `ayther_pose_sub_clear_overrides`, `ayther_pose_sub_free`, `ayther_pose_sub_load_pack`, `ayther_pose_sub_new`, `ayther_pose_sub_resolve`, `ayther_pose_sub_set_cram`, `ayther_pose_sub_set_screen`, `ayther_rom_patch_error`, `ayther_script_free`, `ayther_script_get_audio_overrides`, `ayther_script_get_shader_params`, `ayther_script_get_sprite_overrides`, `ayther_script_get_tile_overrides`, `ayther_script_load_string`, `ayther_script_new`, `ayther_script_on_frame`, `ayther_script_set_pack`, `ayther_script_update_audio`, `ayther_script_update_sprites`, `ayther_script_update_tiles`, `ayther_scroll_unwrapper_free`, `ayther_scroll_unwrapper_last_step`, `ayther_scroll_unwrapper_new`, `ayther_scroll_unwrapper_push`, `ayther_sf2_all_notes_off`, `ayther_sf2_bake`, `ayther_sf2_control`, `ayther_sf2_free`, `ayther_sf2_list_presets`, `ayther_sf2_new`, `ayther_sf2_new_shared`, `ayther_sf2_note_off`, `ayther_sf2_note_on`, `ayther_sf2_preset_list`, `ayther_sf2_program`, `ayther_sf2_render`, `ayther_sf2_trim_cache`, `ayther_sonic_read_velocity`, `ayther_sonic_read_xy`, `ayther_soundfont_normalize_file`, `ayther_sprite_hasher_clip_count`, `ayther_sprite_hasher_free`, `ayther_sprite_hasher_get_clip`, `ayther_sprite_hasher_get_occurrences`, `ayther_sprite_hasher_new`, `ayther_sprite_hasher_process_sprites`, `ayther_sprite_hasher_process_vram`, `ayther_sprite_hasher_reset_animation_grouper`, `ayther_sprite_hasher_unique_count`, `ayther_sprite_sub_add_override`, `ayther_sprite_sub_add_override_ref`, `ayther_sprite_sub_clear_overrides`, `ayther_sprite_sub_free`, `ayther_sprite_sub_load_pack`, `ayther_sprite_sub_new`, `ayther_sprite_sub_resolve`, `ayther_subsystem_count`, `ayther_subsystem_name`, `ayther_tile_brightness_factor`, `ayther_tile_hasher_dump_toml`, `ayther_tile_hasher_free`, `ayther_tile_hasher_get_occurrences`, `ayther_tile_hasher_new`, `ayther_tile_hasher_process_frame`, `ayther_tile_hasher_unique_count`, `ayther_tile_mean_level`, `ayther_tile_shape_hash`, `ayther_tile_sub_add_override`, `ayther_tile_sub_begin_frame`, `ayther_tile_sub_clear_overrides`, `ayther_tile_sub_free`, `ayther_tile_sub_load_pack`, `ayther_tile_sub_load_pack_named`, `ayther_tile_sub_lookup`, `ayther_tile_sub_new`, `ayther_tile_sub_resolve`, `ayther_tween_begin_frame`, `ayther_tween_clear`, `ayther_tween_clear_overrides`, `ayther_tween_free`, `ayther_tween_load_pack`, `ayther_tween_new`, `ayther_tween_resolve`, `ayther_tween_set_override`, `ayther_widescreen_gate_eval`, `ayther_widescreen_gate_free`, `ayther_widescreen_gate_new`, `AytherAnimFrame`, `AytherAudioActive`, `AytherAudioActiveSub`, `AytherAudioEvent`, `AytherAudioEventDetector`, `AytherAudioEventSub`, `AytherAudioHasher`, `AytherAudioOccurrence`, `AytherAudioOverride`, `AytherAudioSub`, `AytherAudioSubstitutor`, `AytherAudioWrite`, `AytherBatchEventDetector`, `AytherBgStitcher`, `AytherCompat`, `AytherCredits`, `AytherEventSub`, `AytherGameProfile`, `AytherGeometricTween`, `AytherPackBuilder`, `AytherPackReport`, `AytherPackWatcher`, `AytherPcmEvent`, `AytherScriptEnv`, `AytherScrollUnwrapper`, `AytherSf2`, `AytherShaderParams`, `AytherSpriteHasher`, `AytherSpriteOccurrence`, `AytherSpriteOverride`, `AytherSpriteSub`, `AytherSpriteSubstitutor`, `AytherTileHasher`, `AytherTileOccurrence`, `AytherTileOverride`, `AytherTileSub`, `AytherTileSubstitutor`, `AytherTransform`, `PoseSetSubstitutor`, `struct`, `TweenPlayer`, `WidescreenGate`

_The installed header (`include/ayther/ayther_core_ffi.h`) carries the full documentation of every symbol._

---

<a id="ayther_envh"></a>

## ayther_env.h

ayther_env.h — getenv with a fallback to the legacy AETHER_ prefix (code
rebrand 2026-07-25): older scripts and harnesses that export AETHER_* keep
working unchanged. ALWAYS use this for AYTHER_* variables.

**Declares:** `ayther`, `env_get`

_The installed header (`include/ayther/ayther_env.h`) carries the full documentation of every symbol._

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

<a id="ayther_rankh"></a>

## ayther_rank.h

ayther_rank.h — the resolution LADDER: which entity wins when several match
the same content.

Product rule (2026-07-26): matching ALWAYS prioritises from HIGHER to LOWER
complexity. The winning entity CLAIMS its coverage, and the lower-ranked
entities contained within it are not drawn — they are not covered up by draw
order, they are not even emitted.

Why here and not in the renderer: lane order is a consequence, not the
decision. When priority lives in the draw order, two entities end up painting
the same region and the result depends on which one goes last — which is
exactly the bug the Picture had (the Prop quads of that screen were drawn ON
TOP OF the Picture that already contained them).

Before this there was no notion of priority BETWEEN types: six matchers ran
in isolation, each one wrote its own FrameView buffer and the renderer drew
them all. The only ladders were INTRA-domain, with two incompatible claim
arrays: `claimed[]` over sprite occurrences and `consumed[]` over plane
cells.

**Declares:** `ayther`, `outranks`, `rank_name`, `uint8_t`

_The installed header (`include/ayther/ayther_rank.h`) carries the full documentation of every symbol._

---

<a id="ayther_recordingh"></a>

## ayther_recording.h

ayther_recording.h — deterministic gameplay recording (.arp).  Ayther R7.

An Ayther Replay Package is NOT a video: it is the deterministic *input* to
the Lab. A recording = an initial savestate + the per-frame input stream.
Replaying = restore the state and re-inject the inputs → the exact same
gameplay, frame for frame, scrubbable (see lab.md §4, lab-engine-split §7).

  take.arp = game_id + name
           + initial savestate (zstd-compressed on disk)
           + input stream (one RetroPad bitmask per frame)

The occurrence history ({slot,hash,anim_group} per frame) + trim marks land
in R7b; this is the recording/replay foundation.

In memory the initial state is kept RAW (ready to unserialize); compression
happens only at save() time. The motor (AytherSession) records into this and
replays from it via replay_seek().

**Declares:** `ayther`, `AytherRecording`, `FrameStat`

_The installed header (`include/ayther/ayther_recording.h`) carries the full documentation of every symbol._

---

<a id="ayther_rendererh"></a>

## ayther_renderer.h

AytherRenderer — the motor's visual (HD) layer (R3).

Consumes a FrameView (the deterministic CPU output of AytherSession::step())
+ a borrowed VkContext, and renders the HD frame into an offscreen VkImage
(VkRenderTarget). The frontend then presents that image:
  - ayther_play blits it to its swapchain;
  - ayther_lab samples it in an ImGui viewport.

Kept SEPARATE from AytherSession on purpose: the session stays Vulkan-free /
headless (CI, determinism, future mobile); the renderer is the swappable GPU
layer. See docs/architecture/r3-render-to-texture.md.

Lifecycle: init(ctx, w, h) → render(ctx, cmd, fv) per frame → shutdown(ctx).
Single-owner; driven from the same thread as the session.

R3.0: scaffold — owns the offscreen target; render() clears it. The emu-frame,
HD-tile, sprite and post-process passes land in R3.1 / R3.2.

**Declares:** `AyArchive`, `ayther`, `AytherLayerStack`, `AytherRenderer`, `FrameScratch`, `FrameView`, `SceneElement`, `VkContext`

_The installed header (`include/ayther/ayther_renderer.h`) carries the full documentation of every symbol._

---

<a id="ayther_resulth"></a>

## ayther_result.h

ayther_result.h — no-throw error model for the runtime / FFI boundary.

The FFI never propagates exceptions or panics across the binary boundary
(see docs/architecture/ayther-engine.md §4.1). Expected failures (corrupt
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

Motor / frontend boundary (see docs/architecture/ayther-engine.md §7):

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

<a id="ayther_unique_handleh"></a>

## ayther_unique_handle.h

ayther_unique_handle.h — RAII for opaque Rust handles.

No raw opaque pointer from ayther_core lives loose in C++ (see
docs/architecture/ayther-engine.md §4.1, §10). Every handle is wrapped at
creation so Rust frees it deterministically on scope exit — even through an
early return — which matters during hot-reload / scene restart.

The deleter is the matching `_free()` function, carried as a *stateless*
template parameter, so the wrapper is exactly pointer-sized (zero overhead).

Usage:
  using TileHasherPtr =
      ayther::unique_handle<AytherTileHasher, &ayther_tile_hasher_free>;
  TileHasherPtr h{ ayther_tile_hasher_new() };   // freed automatically

**Declares:** `ayther`, `handle_deleter`

_The installed header (`include/ayther/ayther_unique_handle.h`) carries the full documentation of every symbol._

---

<a id="ayther_versionh"></a>

## ayther_version.h

Canonical AYTHER release and compatibility version contract.

AYTHER_VERSION_* identifies the product release shared by Cargo, CMake, the
native SDK, the engine compatibility check, and the Lua API. The remaining
values are independent protocol revisions and do not follow SemVer.

_The installed header (`include/ayther/ayther_version.h`) carries the full documentation of every symbol._

---

<a id="ayther_videoh"></a>

## ayther_video.h

ayther_video.h — video decoder for the Kinematic.

WHAT IT IS: a VP9 clip in an IVF container, decoded to BGRA8 ready to upload
into a texture. It knows nothing about Vulkan or the session: you ask it for
a frame index and it returns pixels.

WHY VP9/IVF AND NOT FFmpeg — it is not taste, it is the project's GPL
boundary. `ayther_engine` is a STATIC library; the FFmpeg core is LGPL-2.1+,
so linking it would force dynamic distribution or shipping relinkable
objects, and any --enable-gpl component would make it GPL. libvpx is BSD-3 +
patent grant. IVF is 32 bytes of header and 12 per frame, which means the
demuxer fits in this file and drags in no libavformat. The ENCODER remains an
EXTERNAL ffmpeg.exe from PATH (lab/src/app/ffmpeg_pipe.h): a separate
process, no linking, no licence question.

WHY THE HEADER DOES NOT INCLUDE vpx: if `vpx/vpx_decoder.h` came in here, the
libvpx include dir would have to be PUBLIC in the engine CMake, and the Lab,
the runtime and the tools would start depending on an OPTIONAL library.
Pimpl.

WITHOUT libvpx (AYTHER_HAVE_VPX off) this still compiles: open() returns
false with a reason and validate() rejects. A contributor without libvpx is
not blocked, and the bake never bakes a video without validating it.

STREAMING: the clip does NOT reside in RAM. It reads from a `VideoSource` —
the pack by range, or a file — and keeps only the packet index, the current
packet and the converted frame. It used to copy the whole .ivf, and that was
the real reason for the bake's 32 MB per-video cap: not the format, but that
`ayther_pack_read` is all-or-nothing. With the cap removed, an 8K clip takes
the same space as a 3 s one.

**Declares:** `ayther`, `Impl`, `video_i420_to_bgra_px`, `video_index_build`, `video_index_frames`, `video_index_path`, `video_probe`, `video_source_from_file`, `video_source_from_pack`, `VideoClip`, `VideoFrameView`, `VideoInfo`, `VideoPlane`, `VideoProbe`, `VideoSource`

_The installed header (`include/ayther/ayther_video.h`) carries the full documentation of every symbol._

---

<a id="cram_paletteh"></a>

## cram_palette.h

cram_palette.h — the Mega Drive CRAM, read (EM-9.4).

The four VDP palette lines: 64 nine-bit colours that decide what colour every
index of every tile is seen as. Everything else in the pipeline —the plane
tile hash, the per-palette variant signature, the tint— leans on this, and
until now the conversion lived loose in three different places inside
`ayther_session.cpp`.

# The format: PACKED, not the bus one

The CRAM the fork publishes comes PACKED —R in bits 0-2, G in 3-5, B in 6-8—
and **not** in the Genesis bus format, which leaves gaps (R=1-3, G=5-7,
B=9-11). Confusing them yields colours that look plausible: everything comes
out at half intensity and hue-shifted, which is worse than coming out plainly
wrong — nobody looks at it twice.

Verified against the game: white = 0x1FF, blue = 0x1E3 → R3 G4 B7.

# From 3 bits to 8: it is NOT `x << 5`

A 3-bit component taken to 8 with a shift never reaches 255: maximum white
would give 224 and the whole image would look washed out. The correct
expansion repeats the bit pattern, which is what makes 7 → 255 and 0 → 0 with
the intermediate values spread evenly.

**Declares:** `ayther`, `cram_c8`, `cram_color`, `cram_color_at`, `cram_line_signature`, `CramColor`

_The installed header (`include/ayther/cram_palette.h`) carries the full documentation of every symbol._

---

<a id="failure_escalationh"></a>

## failure_escalation.h

failure_escalation.h — when to stop trying.

The fallback already prevents a broken asset from cutting the session short:
the original is heard and that is that. What is missing is ESCALATION — a
pack with many broken assets retries each one, every frame, and pays for the
full resolution of something already known not to work. That is the risk
noted here.

THE RULE, which is the only thing this header holds:

  DISTINCT ASSETS are counted, not occurrences.

One broken file that plays a thousand times is ONE problem; twelve distinct
files is a badly assembled pack or a folder that never arrived. Counting
occurrences would shut the subsystem down over a single asset that repeats a
lot — which is exactly the case NOT to punish, because the fallback already
handles it well.

And the count is PER SUBSYSTEM: missing music says nothing about sound
effects, and shutting both down over one would take out what does work.

Header-only and dependency-free: it is tested without a session, without
audio and without a ROM.

**Declares:** `ayther`, `FailureEscalation`, `threshold_`

_The installed header (`include/ayther/failure_escalation.h`) carries the full documentation of every symbol._

---

<a id="libretro_hostayther_apih"></a>

## libretro_host/ayther_api.h

ayther_api.h — COPIA del contrato del Core Fork. NO editar a mano.

AYTHER_API_VERSION: v1.10 — sincronizar con davidlazarte/Genesis-Plus-GX
                    core/ayther/ayther_api.h (release ayther-abi-1.10,
                    core v1.7.4 752a6ff7; guia: docs/ayther_integration_1.9.md §5.1,
                    que sirve para 1.10)

COPIA ENTERA desde 1.9 (2026-08-26; 1.10 el mismo dia). Hasta 1.3-r2 se traia un recorte «solo
lo que se consume»; con seis versiones aditivas encima (SYSTEM, controles en
Mode 4, LINE_STATE, observabilidad, SPRITE_OUTCOME, Z80_RAM) el recorte ya
no ahorra nada y esconde lo que hay para descubrir. Declarar un tipo NO es
consumirlo: lo que el Engine LEE lo dicen sus suscripciones (pide solo los
bits que consume, no AYTHER_SUB_ALL) y sus chequeos de capability.

Tres cambios de SIGNIFICADO de 1.3 a 1.9, sin cambio de firma (guia §2):
 1. 0x10E / fallback_reasons en Mode 4 dice la verdad (ya no arranca en
    UNSUPPORTED_MODE; el Z80 marca CRAM/VRAM a mitad de frame). El Engine
    nunca tuvo un «es SMS → fallback siempre», asi que solo hay que seguir
    confiando en la mascara.
 2. `v_counter` del journal = primera linea que VE el cambio (N+1). El
    Engine no lo usa.
 3. `recompose_multilayer` con eventos de CRAM cambia de salida (la nueva es
    la correcta, pixel-perfect). Medido con abi_multilayer.

Se copia en vez de referenciar el repo del fork desde CMake: un path
cross-repo ataria el build del Engine al checkout del core, y el contrato
es justamente lo que permite que los dos repos evolucionen por separado.
Al actualizarlo, re-copiar entero y anotar el commit de origen arriba.

 (2026-08-12): el contrato SI cambia — entra `ayther_recompose_status`
con los cuatro motivos de rechazo de la recomposicion. Antes cualquier
rechazo salia como UNSUPPORTED y no habia motivo que loguear. Van en -20..-23
y no en el rango natural porque los `AYTHER_RC_ERR_*` INTERNOS del core ya
ocupan -1..-4, donde viven INVALID_ARGUMENT y compania: devolverlos crudos
haria que «no es modo 5» se leyera como «argumento invalido». Es aditivo —
esos valores no se devolvian antes— asi que un caller que solo compara contra
OK no cambia de comportamiento.

 (2026-08-12): dos defectos que hacian que dos campos de este header
publicaran datos falsos: `dirty_patterns` llegaba siempre vacio (fork ) y
`raster_event_count` estaba clavado en 256 (fork ). Y `cram` se leia con
`movaps` sin estar alineado a 16, asi que `ayther_recompose_multilayer`
CRASHEABA el proceso con -O2 (fork ). Medido en `abi_frame_delta` y
`abi_multilayer`.

 (2026-08-12): trae FRAME_DELTA_V1 (poll_frame_delta + dirty_patterns),
el export directo ayther_recompose_multilayer con sus AYTHER_RC_ERR_*, y el
Audio Probe v2. OJO: `ayther_audio_event_v1` paso sus campos a una UNION y
por lo tanto CAMBIO DE TAMANO — el tamano tiene que salir de `event_size` del
propio core y no de un sizeof local.

2026-08-24, ABI 1.2: la MULTICAPA SE MUDO AL DESCRIPTOR. El simbolo suelto
`ayther_recompose_multilayer` ya no se exporta en el perfil standard, asi que
resolverlo con GetProcAddress devuelve NULL contra un core >= 1.2 y la feature
queda muda SIN un error a la vista. Se resuelve con AYTHER_IFACE_HAS y se cae
al export suelto para los cores 1.0/1.1, que siguen valiendo: la ABI es
aditiva y nada obliga a actualizar el binario.

 (2026-08-12): el AUDIO_EVENT sube a schema 2 — el key-on de PCM ahora
lleva `st`/`ls`, que es lo unico que dice QUE SAMPLE suena (env es volumen y
fd es velocidad de reproduccion: ninguno identifica nada). De paso la fuente
PCM deja de mezclar las dos ramas de la union y usa {reg,data} siempre. Este
es el camino por el que entra el chip PCM de Sega CD al detector, que sigue
siendo el DUENO de la identidad: el core da los HECHOS (st, ls, fd, env, pan)
y `audio_event.rs` calcula la firma, igual que con FM y PSG. Ver el comentario
de la union para el empaquetado exacto.

**Declares:** `ayther_audio_event_type_v1`, `ayther_audio_event_v1`, `ayther_audio_source_v1`, `ayther_audio_transport_stats_v1`, `ayther_audio_voice_v1`, `ayther_audio_write_v1`, `ayther_endianness`, `ayther_frame_delta_v1`, `ayther_frame_hash_v1`, `ayther_frame_snapshot_v1`, `ayther_get_interface`, `ayther_interface_v1`, `ayther_journal_event_v1`, `ayther_journal_v1`, `ayther_legacy_memory_id`, `ayther_line_cells_v1`, `ayther_line_header_v1`, `ayther_line_regs_v1`, `ayther_recompose_error`, `ayther_recompose_multilayer`, `ayther_recompose_stats_v1`, `ayther_recompose_status`, `ayther_region_id`, `ayther_region_info_v1`, `ayther_sprite_v1`, `ayther_status`, `ayther_subscription_state_v1`, `ayther_system_v1`, `int32_t`

_The installed header (`include/ayther/libretro_host/ayther_api.h`) carries the full documentation of every symbol._

---

<a id="libretro_hostcore_loaderh"></a>

## libretro_host/core_loader.h

**Declares:** `CoreLoader`

_The installed header (`include/ayther/libretro_host/core_loader.h`) carries the full documentation of every symbol._

---

<a id="libretro_hostlibretroh"></a>

## libretro_host/libretro.h

**Declares:** `bool`, `float`, `int`, `int16_t`, `int64_t`, `retro_api_version`, `retro_audio_buffer_status_callback`, `retro_audio_callback`, `retro_av_enable_flags`, `retro_camera_buffer`, `retro_camera_callback`, `retro_cheat_reset`, `retro_cheat_set`, `retro_controller_description`, `retro_controller_info`, `retro_core_option_definition`, `retro_core_option_display`, `retro_core_option_v2_category`, `retro_core_option_v2_definition`, `retro_core_option_value`, `retro_core_options_intl`, `retro_core_options_update_display_callback`, `retro_core_options_v2`, `retro_core_options_v2_intl`, `retro_deinit`, `retro_device_power`, `retro_disk_control_callback`, `retro_disk_control_ext_callback`, `retro_exec_mem_alloc`, `retro_exec_mem_free`, `retro_fastforwarding_override`, `retro_frame_time_callback`, `retro_framebuffer`, `retro_game_geometry`, `retro_game_info`, `retro_game_info_ext`, `retro_get_memory_size`, `retro_get_proc_address_interface`, `retro_get_region`, `retro_get_system_av_info`, `retro_get_system_info`, `retro_hw_context_type`, `retro_hw_render_callback`, `retro_hw_render_context_negotiation_interface`, `retro_hw_render_context_negotiation_interface_type`, `retro_hw_render_interface`, `retro_hw_render_interface_type`, `retro_init`, `retro_input_descriptor`, `retro_key`, `retro_keyboard_callback`, `retro_language`, `retro_led_interface`, `retro_load_game`, `retro_load_game_special`, `retro_location_callback`, `retro_log_callback`, `retro_log_level`, `retro_memory_descriptor`, `retro_memory_map`, `retro_message`, `retro_message_ext`, `retro_message_target`, `retro_message_type`, `retro_microphone_interface`, `retro_microphone_params`, `retro_microphone_t`, `retro_midi_interface`, `retro_mod`, `retro_netpacket_callback`, `retro_perf_callback`, `retro_perf_counter`, `retro_perf_tick_t`, `retro_pixel_format`, `retro_power_state`, `retro_proc_address_t`, `retro_reset`, `retro_rumble_effect`, `retro_rumble_interface`, `retro_run`, `retro_savestate_context`, `retro_sensor_action`, `retro_sensor_interface`, `retro_serialize`, `retro_serialize_size`, `retro_set_audio_sample`, `retro_set_audio_sample_batch`, `retro_set_controller_port_device`, `retro_set_environment`, `retro_set_input_poll`, `retro_set_input_state`, `retro_set_video_refresh`, `retro_subsystem_info`, `retro_subsystem_memory_info`, `retro_subsystem_rom_info`, `retro_system_av_info`, `retro_system_content_info_override`, `retro_system_info`, `retro_system_timing`, `retro_throttle_state`, `retro_time_t`, `retro_unload_game`, `retro_unserialize`, `retro_usec_t`, `retro_variable`, `retro_vfs_dir_handle`, `retro_vfs_file_handle`, `retro_vfs_interface`, `retro_vfs_interface_info`, `size_t`, `uint64_t`, `uintptr_t`, `unsigned`, `void`

_The installed header (`include/ayther/libretro_host/libretro.h`) carries the full documentation of every symbol._

---

<a id="libretro_hostretro_runnerh"></a>

## libretro_host/retro_runner.h

**Declares:** `RetroRunner`

_The installed header (`include/ayther/libretro_host/retro_runner.h`) carries the full documentation of every symbol._

---

<a id="output_profileh"></a>

## output_profile.h

output_profile.h — OUTPUT profiles.

These are NOT the remastering profiles, and confusing the two would be the
worst outcome here. The remastering ones say **what gets substituted** and
the pack author decides them; these say **how it looks on YOUR screen** and
whoever is playing decides them. A CRT does not change which assets come in,
and a "Faithful" profile does not change whether you own a plasma or a
laptop.

That is why they live in different headers and why the vocabulary keeps them
apart: "remastering profile" versus "output profile".

The profile configures three things: SCALING, SMOOTHING and the presentation
SHADERS. That is everything between the composed frame and the monitor.

Header-only and Vulkan-free: it is tested without a GPU.

**Declares:** `ayther`, `output_profile_by_id`, `output_profiles`, `output_rect`, `OutputProfile`, `OutputRect`, `uint8_t`

_The installed header (`include/ayther/output_profile.h`) carries the full documentation of every symbol._

---

<a id="pano_bandsh"></a>

## pano_bands.h

pano_bands.h — the camera of a Panorama, VOTED PER BAND.

THE PROBLEM. The Panorama models a rigid strip with ONE camera: every visible
cell votes `cam_px = lx*8 - screen_x` and the mode wins. When the plane has
line-scroll —bands that move at different rates within the SAME VDP layer—
no single position explains them all: the cells of the fast band vote against
those of the background. At best the mode wins and the minority band ends up
misplaced; at worst the vote splits and the anchor never settles.

THAT THE CASE EXISTS is measured, not assumed (2026-08-24,
hscroll_bands_probe):

  Golden Axe   3 takes, 40,854 frames   reg $B mode 0   0 bands
  Ecco         1,800 frames             reg $B mode 0   0 bands
  Aladdin      1,800 frames             reg $B mode 0   0 bands
  Sonic 3 & K  1,800 frames             per-line table in 1,766
                                        plane A: 1 band · plane B: 37

Golden Axe is NOT the corpus for this feature —its title-screen clouds were
resolved as two parallaxed Acetates—; Sonic 3 & Knuckles is.

THE SHAPE OF THE SOLUTION. With 37 bands, declaring one drift per strip
(direction 2 of the issue) is not enough: that would be 37 speeds the author
would have to maintain by hand. The vote is PER BAND, which is what the
hardware does.

This file is only the VOTE: it groups and decides, it does not read VRAM,
does not touch Vulkan and does not know what a Panorama is. Like
`widescreen.h`, it can be measured without a GPU and without a ROM — which is
how the banding bug in EM-8.0 was found.

**Declares:** `ayther`, `BandCam`, `pano_band_edges`, `pano_vote_by_band`, `PanoVote`

_The installed header (`include/ayther/pano_bands.h`) carries the full documentation of every symbol._

---

<a id="panorama_coverh"></a>

## panorama_cover.h

panorama_cover.h — the COVERAGE rule of a Panorama.

"Is what is seen at this position the strip, or is it something else drawn on
top of the same plane?" Once the camera has anchored, that has to be answered
cell by cell, and `FrameView.panorama_cover` comes out of that count.

IT LIVES IN A HEADER AND NOT INSIDE THE SESSION because it is a rule of the
strip FORMAT —like `ayther_plane_tile_hash_variants`, which it leans on— and
because the defect it fixes could not be tested without a ROM and a
twenty-minute capture. Here it is tested with three made-up hashes.

THE DEFECT (measured on Sonic 3 & Knuckles f2092). One position of the strip
can have SEVERAL hashes: an animated cell has one per state, and a scroll
that crossed into another zone stacks two sections of the level at the same
position. The index keeps them all —every state has to be able to ANCHOR—
but the PNG keeps ONE (`Cell::last` in the stitcher).

Accepting any of them to verify coverage declares "anchored, 100 % coverage"
over a strip that shows a different section of the level: the exported crop
was Angel Island —sky, water, grass— while the frame was a cave.

WHY IT IS ALMOST NEVER SEEN: the native area corrects itself, because the
live cells the strip did not claim are drawn on top and hide the weak anchor.
What exposes it is widescreen (EM-8.1), where the extended area has nothing
to correct itself with — there you see exactly what the strip holds.

WHAT THE FIX IS NOT: a coverage floor. Both available numbers were tried and
neither separates the cases (Golden Axe extends WELL at 69 %; Sonic 3 & K
extends BADLY at 100 %). A threshold tuned against two data points is a
fragile patch dressed up as a fix.

THE FIX is to align the index with the drawing: coverage is verified against
the hash the strip KEEPS and not against any of the ones that passed through
there. The others are not discarded — they stay in the anchoring index, where
multiplicity helps vote on where the camera is and one extra vote is offset
by the other thirty. What they may not do is decide WHAT IS DRAWN where
nobody is going to correct it.

**Declares:** `ayther`, `panorama_pos_matches`

_The installed header (`include/ayther/panorama_cover.h`) carries the full documentation of every symbol._

---

<a id="parallax_bandsh"></a>

## parallax_bands.h

parallax_bands.h — the level column PER BAND (EM-8.0).

Plane B carries per-band parallax: every entry of the Hscroll table has its
own displacement, so "level column" **is not a single thing on that plane** —
it depends on the row. With a single camera per plane, every band collapses
onto the same columns and they stack on top of one another.

MEASURED on Sonic 2 (`background_spike`, 1200 frames): plane A reconstructed
607 level columns and plane B only **37** —less than one screen— with 45
bands per frame. It was not that art was missing: it was all stacked in the
wrong place.

TWO THINGS THIS FILE LEARNED THE HARD WAY

1. The rule lives here and not inside the loop in `ayther_session.cpp`. The
   first version was buried there, where the stitcher oracle could not see it
   —it calls the stitcher directly— so measuring it moved not a single
   number. It was not wrong: it was not being executed.

2. Subtracting the H of two bands within the same frame is not enough. The
   VDP field is 10 bits and it wraps, and the separation between bands GROWS
   without bound over the course of a level (measured: 17 px against 566 px
   over 1033 px of scroll). Each band needs its own unwrapping, just like the
   plane camera. That is why `BandCameras` has state: a pure subtraction
   cannot know how many times each band wrapped around.

**Declares:** `ayther`, `band_count`, `BandCameras`, `hscroll_base`, `hscroll_mask`, `hscroll_of_line`, `State`, `wrap_px`

_The installed header (`include/ayther/parallax_bands.h`) carries the full documentation of every symbol._

---

<a id="psg_synthh"></a>

## psg_synth.h

PsgSynth — our own SN76489 (PSG), with PER-CHANNEL output. Phase 1.

WHY OUR OWN. ymfm covers the YM2612 but not the SN76489, and the
implementations in circulation (MAME, GPGX itself) are GPL: ayther_engine is
a STATIC library and adding GPL contaminates the whole of it. The chip is
genuinely small —three square-wave generators and an LFSR— so writing it is
cheaper than arguing about licences.

`gpgx-src/core/sound/psg.c` is the reference for BEHAVIOUR (constants, volume
table, noise feedback network), not for code.

INTERNAL RATE. The chip runs at MCLK/15 and divides by 16, i.e. one tick
every 15*16 = 240 M-cycles → 223721.56 Hz with the NTSC clock. That is
exactly 4.2× the YM2612 rate (MCLK/1008), and it is no coincidence:
1008/240 = 4.2.

That detail matters. ALL PSG frequency increments are multiples of 240
M-cycles, so every wave transition lands EXACTLY on a tick boundary —
sampling at 223721 Hz is exact, with no sub-sample jitter and no need for
band-limited synthesis here. Aliasing is handled later, when decimating to
the output rate with the resampler.

**Declares:** `ayther`, `PsgSynth`, `shift_noise`

_The installed header (`include/ayther/psg_synth.h`) carries the full documentation of every symbol._

---

<a id="rewind_bufferh"></a>

## rewind_buffer.h

rewind_buffer.h — compressed savestate ring buffer for rewind (R6).

Holds a sliding window of emulator savestates so the frontend can step the
emulation *backwards*. Each ~1 MB savestate is zstd-compressed (level 1,
~30–60 KB) on push, so a 10-second window at 60 fps fits in ~18–36 MB
instead of ~600 MB raw (see ayther-engine.md §6.1).

Model: push() the current state each frame (END of step). rewind_step()
drops the current state and restores the previous one, so holding rewind
walks back one frame per display frame. Disabled by default — when off,
push() is a no-op and there is zero per-frame cost.

Single-threaded; driven from the emulation thread alongside AytherSession.

**Declares:** `ayther`, `RewindBuffer`, `ring_`

_The installed header (`include/ayther/rewind_buffer.h`) carries the full documentation of every symbol._

---

<a id="voice_routerh"></a>

## voice_router.h

voice_router — the per-voice channel router (Phase 2).

THE INVERSION. Until now substitution was SUBTRACTIVE: the chip played in
full and channels were masked out with a mask derived from event WINDOWS. A
window is a MODEL of the sound; the chip IS the sound, so every instant the
window did not cover while the chip kept playing was a leak — the gaps
between notes and the seam between Sequences are the same defect seen in two
places.

Here the default is flipped: the chip is MUTED and everything heard is
produced by this router. A voice takes over the channel FROM THE CHIP'S OWN
KEY-ON until the end of its tail. There is no window left to get wrong.

THE TWO PIECES
  ChipMirror  — a YM2612 (ymfm) and an SN76489 (PsgSynth) fed with the same
                write log the core receives, producing the 10 channels
                SEPARATELY. It is the substrate: it always runs, whether or
                not the voices take its output.
  ChannelRouter — 10 slots. On every key-on it asks the policy what should
                play and points the slot at that source.

WHY THE MIRROR ALWAYS RUNS: its register state IS the identity of the sound.
If it were switched off while a voice is substituted, on return it would not
know what timbre it was playing with — measured in Phase 0: starting the
synthesiser without the previous state drops the envelope correlation from
0.975 to 0.889.

**Declares:** `ayther`, `begin`, `buf_`, `ChannelRouter`, `ChipMirror`, `gen_until`, `IVoiceSource`, `reset`, `StreamResampler`, `VoiceContext`, `VoicePolicy`, `Ym2612Mirror`, `ymfm`

_The installed header (`include/ayther/voice_router.h`) carries the full documentation of every symbol._

---

<a id="vulkan_backendtile_tex_cacheh"></a>

## vulkan_backend/tile_tex_cache.h

TileTexCache — lazy-load + cache HD tile textures from an .ay pack.

First access per asset_path:  read bytes from pack → decode PNG (stb_image) →
swap R↔B (RGBA→BGRA) → VkTexture init()+upload(). Subsequent accesses return
the cached VkTexture directly. Owned by AytherRenderer.

**Declares:** `AyArchive`, `StagingRelease`, `TileTexCache`, `VkContext`

_The installed header (`include/ayther/vulkan_backend/tile_tex_cache.h`) carries the full documentation of every symbol._

---

<a id="vulkan_backendvk_contexth"></a>

## vulkan_backend/vk_context.h

VkContext — Vulkan instance, physical device, logical device, queues, VMA.

One instance per application lifetime.  Destroyed in reverse creation order
inside shutdown() (also called by the destructor).

Uses vk-bootstrap for the boilerplate-heavy device selection / creation.

**Declares:** `SDL_Window`, `vk_verbose_logging`, `VkContext`, `VmaAllocator_T`

_The installed header (`include/ayther/vulkan_backend/vk_context.h`) carries the full documentation of every symbol._

---

<a id="vulkan_backendvk_indexed_planeh"></a>

## vulkan_backend/vk_indexed_plane.h

VkIndexedPlane — the INDEXED pipeline of our own renderer (R-2).

Instead of decoding tiles into RGBA textures, it uploads the RAW VDP STATE
and resolves the colour in the fragment shader:
  - VRAM (2048 4bpp patterns) → a 512×256 R8_UINT texture (64 tiles per row,
    each tile 8×8 texels = its unpacked colour-index nibble).
  - CRAM (64 packed colours R0-2/G3-5/B6-8) → a 64×1 RGBA8 texture, converted
    with the SAME colour expansion as the core renderer (3 bits → level ×2 →
    RGB565 → 888 by bit replication), so the result is comparable BIT FOR BIT
    against the emulator framebuffer.
  - Every 8×8 px quad carries pattern + palette line + flips through a push
    constant; the shader maps index → colour. Index 0 is discarded (VDP
    semantics: transparent).

Why this way (from the epic): a palette fade no longer invalidates anything
(64 texels change), and a per-element effect is one uniform per quad, not a
new lane. Uploads are INCREMENTAL: a CPU shadow of VRAM/CRAM, and only the
tiles/palette that changed travel to the GPU.

Layout contract (identical to VkSprite): the render target must be in
COLOR_ATTACHMENT_OPTIMAL on entry to draw_cells(); the pass leaves it in
TRANSFER_DST_OPTIMAL. upload_*() are recorded OUTSIDE a render pass.
Ordering contract: at least one upload_vram()+upload_cram() before the first
draw_cells() (the images are born UNDEFINED; a draw without an upload is a
no-op).

**Declares:** `VkContext`, `VkIndexedPlane`, `VkRenderTarget`, `VmaAllocation_T`

_The installed header (`include/ayther/vulkan_backend/vk_indexed_plane.h`) carries the full documentation of every symbol._

---

<a id="vulkan_backendvk_render_targeth"></a>

## vulkan_backend/vk_render_target.h

VkRenderTarget — the motor's offscreen color image (R3).

The HD frame is rendered here instead of straight to the swapchain, so each
frontend can decide what to do with it:
  - ayther_play blits it to its swapchain (present to window);
  - ayther_lab samples it in an ImGui viewport panel.

Usage flags cover every consumer in one image:
  COLOR_ATTACHMENT  — the VkSprite / VkPostProcess graphics passes
  TRANSFER_DST      — the emu-frame + HD-tile vkCmdBlitImage blits
  TRANSFER_SRC      — the frontend blit to the swapchain
  SAMPLED           — the Lab viewport (ImGui) / shader sampling

Owned by AytherRenderer and recreated on resize. The target captures the
device/allocator handles it needs, so destruction is automatic; shutdown()
remains available for deterministic early release.

**Declares:** `release`, `VkContext`, `VkRenderTarget`, `VmaAllocation_T`, `VmaAllocator_T`

_The installed header (`include/ayther/vulkan_backend/vk_render_target.h`) carries the full documentation of every symbol._

---

<a id="vulkan_backendvk_spriteh"></a>

## vulkan_backend/vk_sprite.h

vk_sprite.h — Alpha-blended HD sprite overlay pipeline.  Ayther v0.8.0

Renders HD sprite textures (loaded from a .ay pack) over the emulator
framebuffer with standard src-alpha blending.

## Integration into the frame loop

  // swap must be in TRANSFER_DST_OPTIMAL on entry.
  sprite_pipeline.draw(ctx, swap, subs, n_subs, pack, emu_w, emu_h);
  // swap is back in TRANSFER_DST_OPTIMAL on return.
  VkPresent::finalize(ctx, swap);     // unchanged

## Lifecycle

  sprite_pipeline.init(ctx, swap)          // once after swapchain creation
  sprite_pipeline.rebuild(ctx, swap)       // on SDL_EVENT_WINDOW_RESIZED
  sprite_pipeline.shutdown(ctx)            // before vkDeviceWaitIdle / shutdown

## Render-pass layout path (one barrier pair per draw() call)

  TRANSFER_DST  →  [explicit barrier]  →  COLOR_ATTACHMENT
               →  [render pass, loadOp=LOAD, alpha-blend subpass]
               →  [finalLayout auto-transition]  →  TRANSFER_DST

  VkPresent::finalize() then transitions TRANSFER_DST → PRESENT_SRC_KHR
  as usual — no callers need to be changed.

**Declares:** `AyArchive`, `ayther`, `AytherSpriteSub`, `VkContext`, `VkSprite`, `VkTexture`

_The installed header (`include/ayther/vulkan_backend/vk_sprite.h`) carries the full documentation of every symbol._

---

<a id="vulkan_backendvk_textureh"></a>

## vulkan_backend/vk_texture.h

VkTexture — a single GPU image + staging buffer for CPU→GPU upload.

Used to upload the emulator's software framebuffer (RGB565 / XRGB8888)
every frame.  A persistent staging buffer avoids per-frame allocation.

Usage:
  init(ctx, width, height)        — once
  upload(ctx, cmd, pixels, ...)   — inside a command buffer (begin..end)
  shutdown(ctx)                    — optional early release

The image layout at the end of upload() is
  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL (suitable for blit src).

**Declares:** `release`, `TexImageFormat`, `TexPixelFormat`, `VkContext`, `VkTexture`, `VmaAllocation_T`, `VmaAllocator_T`

_The installed header (`include/ayther/vulkan_backend/vk_texture.h`) carries the full documentation of every symbol._

---

<a id="widescreenh"></a>

## widescreen.h

widescreen.h — which level cells fill the extended area (EM-8.1).

The extra width is NOT drawn from what sits in the live nametable. That has
been measured and it does not add up: on the side you are heading towards the
game streams 1-2 cells ahead, and 16:9 over 224 px asks for 5 per side (7 if
the displayed 4:3 aspect is preserved). Plane A —the gameplay— simply has no
lateral art.

  side source           stale cells    without art
  live nametable            182            185
  level strip                 0            185

"Stale" means art from ANOTHER section of the level: the nametable wraps every
512 px, and reading past it returns a band that does not belong. The stitcher
strip —what each position showed while it was on screen— supplies the real
level.

This file is only the PLAN: which level position goes into each cell of the
extended area. It does not draw, does not read VRAM and does not touch
Vulkan, so it can be measured without a GPU and without a ROM — which is how
the banding bug in EM-8.0 was found.

And the row matters: on plane B each parallax band resolves its own column
(see `parallax_bands.h`). An extended area that asked for a single column per
side would leave gaps exactly where parallax separates the bands.

**Declares:** `ayther`, `Coverage`, `ExtCell`, `widescreen_cols_per_side`, `widescreen_coverage`, `widescreen_plan`, `widescreen_target_width`

_The installed header (`include/ayther/widescreen.h`) carries the full documentation of every symbol._
