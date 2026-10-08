# Replacement composition rules

**Status:** implemented; GPU verification is hardware-dependent

**Last verified:** 2026-10-06

These rules decide which original sprites a replacement covers, at which depth
each part of an HD asset is drawn, and what the renderer does when a frame
cannot be composed. Together they guarantee that an original and its
replacement are never seen together, that a replacement hides only what it
replaced, and that the image does not depend on decode timing.

The rules are generic: none of them names a game, a scene or a character.
Cinematic video follows its own contract, in
[Cinematic plane composition](CINEMATIC_PLANE_COMPOSITION.md). What each frame
ended up doing is reported by the [render observation](RENDER_OBSERVATION.md).

## Rules

### R1. Membership

The pose recognizer returns the member occurrences of every applied pose.
A replacement claims exactly its members and nothing else. The depth of a pose
comes from its members (R2), never from its area, and its anchor is always one
of its own members, never an unrelated sprite.

### R2. Per-member depth and partition

The asset of a pose is drawn in parts, each at the depth of the member it
represents: its link-chain position and its priority.

Depth is joined to the exact canonical parsed-SAT occurrence, not to its SAT
slot. A slot may be reused by different records in one frame, and an identical
record parsed at ranks 9 and 1 has canonical rank 1; neither case may borrow the
first rank previously seen for that slot.

- The parts come from the member rectangles scaled into the asset's space.
- An asset pixel outside every member rectangle (silhouette excess) takes the
  depth of the nearest member; on a tie, the one further in front.
- Partial occlusions and front/behind relations are kept, even where the new
  silhouette exceeds the original, without requiring pixel equality.

The observation reports such a replacement as drawn `partitioned`.

### R3. The VDP sprite layer

1. The order between sprites is resolved by the link chain: the first one
   wins, whatever its priority.
2. Each winning pixel is then composed against the planes by its priority
   bit: B low, A low, low-priority sprites, B high, A high, high-priority
   sprites, with the window in its place.

Replaced and original sprites take part under the same rules. The renderer
implements this with a depth buffer in the render target. Sprite and plane
pipelines write the chain depth through a push constant, and the sprite
fragment shaders discard what lies behind.

### R4. HD lanes in their pass

Low-priority HD backgrounds (plane tiles, Pictures and tile substitutions) are
drawn at the position of their plane, before the low-priority sprites. A plane
set is split by the priority of its cells, so each half is drawn in its own
pass.

### R5. Visibility of the originals

Only the sprites the core drew are drawn:

- the framebuffer judge compares each candidate with the core's image, sampling
  only on-screen pixels and accounting for occlusion;
- the per-line sprite limits are modelled (H40: 20 sprites and 320 pixels per
  line; H32: 16 and 256), as well as the mask of a sprite at x = 0. The limit
  follows the current core option (`genesis_plus_gx_no_sprite_limit`);
- duplicates of the parsed sprite list are removed.

Sprites partly off screen are kept, so that a pose at the edge matches
consistently.

### R6. Deterministic residency

- A member is never claimed unless its replacement's texture is resident.
- Synchronous textures are the default
  (`AytherRenderer::set_synchronous_textures(true)`). A frame that needs a
  texture that is not resident decodes and uploads it before composing.
  Turned off, the decode runs in the background and the original is drawn
  until the texture lands, which authoring tools may prefer.
- While preparing the session, the host can prewarm the pack's catalog:
  `AytherRenderer::prewarm_textures(ctx, pack, session.catalog_texture_assets())`,
  within `kPrewarmBudgetBytes`. The `PrewarmReport` lists the assets that
  failed in `missing_assets`.
- The catalog covers pose assets and variants, per-sprite assets, plane sets
  and sequence steps, panoramas and screens.
- Flips are drawn with texture coordinates and never create another texture.

### R7. Pose without an asset

A catalog pose without a valid asset does not claim its members. The pack
validator reports it as `pose.asset_missing` or `pose.asset_unreadable`.

### R8. Frames that cannot be composed

- The compositor draws per-line horizontal scroll and per-column vertical
  scroll by bands, clipping each plane element to its band. Those frames are
  composed normally.
- A frame with raster writes in mid-screen (`raster_split`) loses HD only
  in the bands of lines those writes touch (decision DI-17). The session
  finds them from the core's per-line raster journal (registers, CRAM, VSRAM
  and hscroll writes, compared with the state the frame ends with) and, for
  pattern writes the journal does not record, from the lines where the core's
  image differs from the frame the core recomposes from its final state.
  `FrameView::raster_bands` carries them.
- The journal contains active writes only: a pre-frame register snapshot does
  not reveal changes made and restored in vblank, and an event does not carry
  the value it replaced. Consequently every line before the first active
  write to a register, CRAM entry or VSRAM word is banded conservatively. A
  non-final value then remains banded until the next write to the same address.
- Horizontal-scroll localization additionally requires
  `AYTHER_REGION_LINE_REGS` from the same frame generation. Its exact `reg11`
  and `hscb` values select the lines that consumed each word even when the
  layout changed entirely in vblank. Missing, stale, truncated or overflowed
  LINE_STATE keeps the complete core frame. The pinned fork also has legacy
  Z80/DMA paths that put a single VSRAM or hscroll byte into a word-shaped
  journal event without a width field; an odd address or byte-shaped payload
  is therefore ambiguous and uses the complete core frame rather than
  inventing the neighbouring byte. Equality uses the visible ten scroll bits.
- The frame snapshot, raster journal and LINE_STATE read must share one exact
  snapshot generation. Output-only mute/dim controls are restored before that
  snapshot is captured so they cannot invalidate later frame-scoped reads.
  If `AYTHER_SYSTEM_GEOMETRY_PENDING` is set, VDP_REGS already describes the
  following frame; no raster reason is localized from it and the emitted frame
  stays wholly on the core image (`FrameView::scene_dirty` bit 5).
- A sprite parsed before a mid-frame SAT rewrite can remain in the cumulative
  scene after its final SAT slot changes. The session compares position, size,
  the complete visual attribute (pattern, flips, palette and priority), and
  the slot's reachability and rank in the final H32/H40 link chain. The fork
  does not publish the scanline lifetime of a historical occurrence, and RGB
  coincidence cannot prove that it represents the same HD identity. Therefore
  any visible mismatch makes the complete frame non-composable; a hidden
  mismatch does not. This is a conservative whole-frame exception to DI-17
  when the affected band is not observable: it can withdraw HD for one frame,
  so the whole-take O3 continuity oracle must cover it. It fixes Toma 3 frame
  647 without discarding valid cumulative occurrences such as Aladdin's
  mid-frame SAT rewrites or guessing a spatial band from unrelated pixels.
  If the same parsed record appears at two chain ranks, its minimum rank still
  matches Rust's canonical occurrence order but the identity is marked
  ambiguous and cannot authorize the final SAT for a raster frame.
  Mode-5 SAT height is encoded in bits 8-9 and width in bits 10-11 of the
  size/link word; asymmetric sprites are tested so swapping those fields cannot
  silently turn a current sprite into a stale one.
  The matcher is intentionally limited to normal Mode 5: Mode 4 uses another
  SAT layout and interlace mode 2 uses ten-bit Y coordinates and different
  attributes. A visible raster sprite in either mode keeps the complete core
  frame until a mode-specific identity is available. A first active R5/R12
  write also makes the pre-write SAT selection unknown because vblank writes
  are not journaled; cumulative visible sprites are validated conservatively.
- In those bands the renderer shows the core's image: the originals, without
  HD assets. The rest of the frame is composed as usual.
- Decision DI-20 refines this. A band pixel that the core drew exactly as in
  the previous frame, with its 8 neighbours unchanged too, was not touched by
  the writes: it keeps the previous frame's composed HD. Only the changed
  pixels (and their neighbours, so no fringe of a slightly larger HD tile
  survives) show the core's image. The renderer carries only from frame k-1
  composed whole (no band, no whole-frame fallback), with the same geometry
  and no sprite of either frame crossing the band; above 4096 runs per frame,
  or otherwise, the band stays the core's image (`session/band_carry.h`).
  Without a pack the previous frame is the core's own image, so O1 is
  unchanged.
- Decision DI-21: when the core's recomposition from its final state is
  available (it is requested for any raster reason), a touched line stays in a
  band only if the core's image differs from that recomposition there. This
  bounds the conservative lines before a first CRAM, VSRAM or hscroll write,
  hscroll without exact LINE_STATE and byte-shaped events. The sprite-identity
  safeguards are unchanged: a stale visible occurrence still keeps the
  complete core frame.
- Decision DI-22: with HD (a pack, or substitutions in the frame), a band
  caused only by pattern writes (VRAM, with or without DMA) is composed from
  the frame's final state like the rest of it; text or tiles written in
  mid-screen show at most one frame early. Without HD the band remains the
  core's image, so O1 is unchanged. A replacement whole
  inside the bands reads `assigned_not_applied` with the reason
  `frame_not_composable` in the observation; one outside them is drawn.
- When the writes cannot be placed on lines (a dropped journal event, a
  reason the journal does not record and no recomposition, missing exact
  LINE_STATE, an ambiguous byte-shaped word event, or an unsupported sprite
  identity mode), the whole frame is presented with the originals only, as
  before.
- `AYTHER_OVERFLOW_PARSED_SPRITES` means the core's cumulative sprite capture
  is incomplete. Its prefix is discarded, a valid empty ABI view remains
  authoritative over the legacy interface, and `FrameView::scene_dirty` bit 4
  makes the renderer show the complete core frame. This avoids composing a
  scene whose depth, claims or historical SAT identities are only partial.
- A frame that ends with the display off (VDP register 1, bit 6) is the
  core's image whole, reported as `other` unless raster writes come first
  (`FrameView::scene_dirty` bit 3). The VDP shows the backdrop only, and
  while the display is off the game can change any VDP state without the
  journal seeing it, so the lines drawn before the display went off cannot
  be checked against the final state either. No HD lane is recognized or
  published for it: no Panorama anchors (nor fixes its tint reference), no
  Cuadro, plane set or tile replacement is drawn, and sprite replacements
  read `assigned_not_applied(frame_not_composable)`. A game that turns the
  display off at the bottom of every frame would lose its HD; Toma 3 has
  none (its display-off frames are black).

An original and its replacement are therefore never visible at the same time
on the same lines.

### R9. Retiring a pack

`set_pack("")` and any pack change clear everything the old pack defined:

- plane sets and sequences, screens, panoramas and kinematics;
- the enhance lists and the clocks that pointed at them;
- the palette luminance peak (the E1 tint reference).

`AytherSession::pack_derived_state()` reports what remains. The renderer's
texture caches belong to the host, which calls
`AytherRenderer::evict_pack_textures()` on a pack change.

## Verification

Each rule has its regression test, written before the fix:

| Rule | Tests |
| --- | --- |
| R1 | `pose_anchor_test`, `pose_membership_paths_test`, `pose_anchor_order_test` |
| R2 | `pose_depth_test`, `pose_partition_session_test`, `pose_partition_gpu_test` |
| R3 | `vdp_sprite_order_test`, `sprite_layer_order_test` |
| R4 | `hd_lane_order_test`, `plane_set_split_test` |
| R5 | `framebuffer_judge_test`, `sprite_line_limits_test` |
| R6 | `texture_residency_test`, `texture_residency_gpu_test`, `texture_sync_decode_test`, `pack_prewarm_test`, `missing_asset_test`, `sprite_flip_uv_test` |
| R7 | Rust tests of `vram_sprite` and `pack_validate` |
| R8 | `plane_bands_test`, `scroll_tables_test`, `scroll_compose_gpu_test`, `raster_frame_test`, `raster_bands_test`, `raster_band_test`, `frame_composability_test`, `frame_view_digest_test`, `parsed_sprites_empty_session_test`, `display_enable_session_test`, `band_carry_test`, `band_carry_gpu_test` |
| R9 | `pack_removal_test` |

The GPU tests carry the `gpu` label and run under the `windows-native-gpu`
preset. `tools/render_probe` checks the rules over a whole take: O1 compares
the composition without a pack against the core image, O2 checks the
invariants with a pack, and O3 (`--check o3`) checks continuity: from one
frame to the next, a block of the composed image may only change where the
core's image changes too, allowing for motion. `--no-images` lets O3 scan a
whole take.
