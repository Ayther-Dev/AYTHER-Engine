# Render observation

**Status:** implemented; contract 1.0

**Last verified:** 2026-10-03

The render observation tells a host, frame by frame, which sprite occurrences
the Engine detected, which HD replacement claimed each of them, why an
assigned pose was not applied, how the renderer drew each replacement, and
whether the frame could be composed at all. Replay QA uses it to explain what
appears on screen without reading Engine internals.

The contract lives in the installed header `ayther/engine/render_observer.hpp`,
namespace `ayther::engine::render_observation`. Its version
(`contract_version`, currently 1.1; a 1.0 consumer is still served) is
independent of the Engine release, the
audio observation contract, the core ABI and the replay format.

## Installing an observer

1. Derive from `RenderObserver` and implement
   `on_render_frame(const RenderFrameView &) noexcept`.
2. Set `AytherSession::Config::render_observer` before `AytherSession::create`.
   The caller owns the observer and keeps it alive until the session is
   destroyed.
3. Each frame: `step()`, render it, then call
   `AytherSession::publish_render_observation(&draw)` with
   `AytherRenderer::last_draw_report()` before the next `step()`. A host that
   does not render (headless) passes `nullptr`: the draw outcome and texture
   state of every replacement are then `unknown`.

With no observer installed the Engine builds nothing and the `FrameView` is
unchanged. `publish_render_observation` is a no-op in that case.

The callback runs synchronously on the session's thread. Every span and
string view borrows producer storage for the duration of the call only:
consumers copy what they keep.

## What a frame reports

`RenderFrameView` carries the frame position, its composability, the
occurrences and the replacements.

### Composability

| Value | Meaning |
| --- | --- |
| `composable` | The frame is composed element by element. |
| `raster_split` | Raster writes in mid-screen split the frame; it is presented with the originals only (R8). |
| `fade` | The authoring dim of an authoring tool's animation view is on. Replacements are still drawn. |
| `line_hscroll` | Per-line horizontal scroll. Since R8 the compositor draws the scroll bands, so the session reports such frames as `composable`; the value stays in the contract for producers that cannot. |
| `column_vscroll` | Per-column vertical scroll, with the same treatment as `line_hscroll`. |
| `other` | Any other reason that prevents composition, such as a frame that ends with the display off (VDP register 1, bit 6). |

See [Replacement composition rules](REPLACEMENT_COMPOSITION.md#r8-frames-that-cannot-be-composed)
for what the renderer does with each value.

### Occurrences

Each `OccurrenceView` is a sprite occurrence of the frame. `OccurrenceId::index`
is unique within the frame; the SAT slot and the link-chain position are kept
for meaning but may repeat on some producers. It also carries the identity
hash, the native screen rectangle and the VDP priority bit.

| `status` | Meaning |
| --- | --- |
| `replaced` | Member of a replacement that claimed it; `replacement` is its index. |
| `original_unassigned` | Drawn as the original sprite: no assignment matched. |
| `assigned_not_applied` | An assignment exists but its replacement was not used. |
| `hidden_by_author` | Hidden on purpose by an authoring tool. |

For `assigned_not_applied`, `not_applied_reason` is one of `texture_pending`,
`texture_failed`, `frame_not_composable`, `member_hidden` or `hd_off`, or
`unknown` when the Engine cannot tell. `pose` holds the assigned pose key.

### Replacements

Each `ReplacementView` has a `kind` (`pose`, `sprite`, `plane_set`, `screen`
or `panorama`), its pose key and asset path, and `members`: exactly the
occurrences it claimed, never one of another replacement.

When the host published a draw report, `render_availability` is `known`
and:

- `draw` is `in_pass` (drawn in the sprite pass), `partitioned` (a pose drawn
  per member, R2), `lane` (drawn in an HD lane) or `discarded`;
- `texture` is `ready`, `pending` or `failed` at the end of that render.

### Limits

At most `max_occurrences` (256) occurrences and `max_replacements` (256)
replacements are listed per frame. Above that the spans stop at the limit and
`occurrences_total` and `replacements_total` keep the real count: an excess
is reported, never truncated silently.

## Aligning frames with audio

`RenderFrameView::frame` carries the emulation frame. The audio observation
reports the same frame's start on the device line as
`audio_frame_output_boundary`, so a host can relate what it presented with
when that frame's audio left the device.

### Layers and overlays (contract 1.1)

`RenderFrameView::layers` lists the layers of the stack the frame was drawn
with, back to front: `kind` (`plane_b`, `plane_a`, `window`, `sprites`,
`tile_subs`, `video`, `picture`, `panorama`, `plane_tiles_lo`, `sprites_hd`,
`entities`, `animations`, `foreground`, `plane_tiles_hi` or `overlay`), its
`name`, its `stack_index` and whether it is `visible`. For an overlay (a
Custom layer, such as a pack's Acetato) `gated` says whether it shows only
while a Cuadro is present, `gate_open` whether that Cuadro is present in this
frame, and `drawn` whether its sheet was drawn. The list comes with the draw
report and is empty without one.

## Draw report

`AytherRenderer::last_draw_report()` returns a `DrawReport` for the last
`render()`: the emulation frame, whether HD was enabled, and one
`ReplacementDraw` per sprite substitution, index-aligned with the frame's
replacements. The spans stay valid until the next `render()`. With HD off,
every replacement stays unapplied with the reason `hd_off`.

## Verification

- `tests/contracts/render_observer_header.cpp`: the header compiles on its own
  and its values are stable.
- `tests/unit/render_observation_builder_test.cpp` and
  `tests/unit/frame_composability_test.cpp`: membership, reasons and
  composability on synthetic frames.
- `tests/integration/render_observation_session_test.cpp`: the observation of
  a real session, with and without a draw report.
- `tests/integration/render_observation_cost_test.cpp`: the cost with and
  without an observer.
- `tools/render_probe` consumes the observation for its O2 invariants. Its
  invariant 6a reads `FrameView::sprite_occ_core_drawn`, the session's R5
  verdict per occurrence; 6b compares a replacement's members by identity
  and layout (flips and relative positions), and accepts a hand-off: every
  member now belongs to one other drawn replacement.
