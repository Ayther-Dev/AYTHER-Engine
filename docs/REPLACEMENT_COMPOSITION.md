# Replacement composition rules

**Status:** implemented; GPU verification is hardware-dependent

**Last verified:** 2026-10-03

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
- A frame with raster writes in mid-screen (`raster_split`) is presented with
  the originals only, without HD assets. Every assigned pose then reads
  `assigned_not_applied` with the reason `frame_not_composable` in the
  observation.

An original and its replacement are therefore never visible at the same time.

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
| R8 | `plane_bands_test`, `scroll_tables_test`, `scroll_compose_gpu_test`, `raster_frame_test` |
| R9 | `pack_removal_test` |

The GPU tests carry the `gpu` label and run under the `windows-native-gpu`
preset. `tools/render_probe` checks the rules over a whole take: O1 compares
the composition without a pack against the core image, and O2 checks the
invariants with a pack.
