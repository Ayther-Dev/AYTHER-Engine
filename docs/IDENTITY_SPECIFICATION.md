# Pack identity specification

**Status:** implemented public contract; pre-release and not frozen

**Last verified:** 2026-08-29

AYTHER binds every replacement to an identity derived from observed emulated
state. This document specifies those identities precisely enough for an
independent implementation to reproduce them without reading AYTHER source.
The reference implementation is under `core/src/`; its known-answer tests
(KATs) are normative when prose and code appear to disagree.

> [!WARNING]
> Identity changes are pack-compatibility changes. A failing published KAT is
> not a prompt to update an expected number. It is evidence that an identity
> changed and that existing packs may no longer match. Investigate, document
> the compatibility impact, and provide migration tooling before accepting it.

This contract is public so community tooling can author compatible packs.
AYTHER Lab may accelerate authoring, but it must never be the only way to
produce a pack. This interoperability promise does not grant rights to ROMs,
game assets, emulator cores, or derivative content. See
[Legal and distribution boundaries](LEGAL_AND_DISTRIBUTION.md).

## Global hash conventions

All identities in this document are unsigned 64-bit values. TOML serializes
them as `"0x%016llx"`; readers may also accept the hexadecimal digits without
the prefix.

Two hash families are used:

- `xxh3_64`, from `xxhash-rust::xxh3::xxh3_64` with seed zero, for sprite,
  pose, palette-variant, and animation-group identities;
- FNV-1a-64 with AYTHER's nonstandard seed for audio and plane-tile identities.

```text
SEED  = 0x14650FB0739D0383
PRIME = 0x00000100000001B3  (1,099,511,628,211)
mix(h, b) = (h XOR b) * PRIME, wrapping modulo 2^64
```

> [!CAUTION]
> `SEED` is not the canonical FNV offset basis
> `0xCBF29CE484222325`. Using the canonical value produces incompatible
> identities even though the implementation otherwise looks correct.

Mega Drive / Genesis conventions used below:

| Concept | Contract |
|---|---|
| Tile | 8×8 pixels, 4-bpp planar, 32 bytes, two pixels per byte |
| CRAM | Four lines of 16 words; packed 9-bit color: R bits 0–2, G 3–5, B 6–8 |
| Planes | `0 = A`, `1 = B`, `2 = Window`; mask bits 0, 1, and 2 respectively |
| Sprite coordinates | Top-left screen position is the raw SAT value minus 128 |
| Visible area | 256/320 × 224/240 for H32/H40 × V28/V30 |
| VRAM byte order | The fork exposes a word-swapped buffer; each algorithm below states whether it applies `^ 1` |

## Sprite hash

Reference: `core/src/vram_sprite.rs`.

For `w_tiles` and `h_tiles` in `1..=4`:

```text
buf = []
for t in 0 .. (w_tiles * h_tiles):
    tile = vram[(tile_idx + t) * 32 .. +32]  # no ^1 correction
    for byte in tile:
        buf.push((byte >> 4) & 0x0F)         # high nibble first
        buf.push(byte & 0x0F)
hash = xxh3_64(buf)
```

The decoded buffer contains 64 palette indices per tile.

- The hash is palette-blind: CRAM colors and the palette-line index do not
  participate.
- It is flip-invariant by construction: H/V flips are not applied while
  decoding. Orientation travels separately with the occurrence.
- A tile outside VRAM contributes 64 zero bytes; the input is never truncated.
- Position, SAT slot, link, priority, palette, flips, and explicit dimensions
  do not participate. Dimensions affect only the input length.
- An occurrence is discarded when
  `x <= -(w*8) || y <= -(h*8) || x >= 336 || y >= 240`.

SAT words are reconstructed from the word-swapped buffer. The decoded fields
are:

```text
screen_y = (w0 & 0x03FF) - 128
h_tiles  = ((w1 >> 10) & 3) + 1
w_tiles  = ((w1 >>  8) & 3) + 1
link     = w1 & 0x7F
priority = w2 >> 15
palette  = (w2 >> 13) & 3
vflip    = (w2 & 0x1000) != 0
hflip    = (w2 & 0x0800) != 0
tile_idx = w2 & 0x07FF
screen_x = (w3 & 0x01FF) - 128
```

The current parser scans all 80 SAT slots and decodes words explicitly. Older
saved sprite assignments produced before that correction may require
re-authoring because the previous parser could hash the wrong graphic.

### Pose identity

`pose_key` preserves capture order:

```text
buf = concat(hash.to_le_bytes() for hash in captured_hashes)
pose_key = xxh3_64(buf)
```

Do not sort. A one-sprite pose intentionally has the same content identity on
the single-substitution and pose paths.

### Animation group identity

For each SAT slot, the grouper maintains a rolling 64-frame window. Members are
hashes observed at least twice; at least two distinct members are required. It
recomputes every 16 frames and rejects a group when one pose accounts for more
than 70 percent of observations.

```text
members.sort()
anim_group_id = xxh3_64(concat(hash.to_le_bytes() for hash in members))
```

`0` means no group. Unlike `pose_key`, member order is deliberately discarded.
Do not use `anim_group_id` as a stable playback key: the rolling member set may
change as an animation transitions. Playback resolution uses pose identity.

### Palette-variant signature

Reference API: `ayther_palette_signature(cram, line, slots)`.

```text
buf = []
for slot in 0 .. 16, ascending:
    if slots & (1 << slot):
        color = cram_words[(line & 3) * 16 + slot] & 0x01FF
        buf += [color & 0xFF, color >> 8]  # little-endian
signature = xxh3_64(buf)
```

Out-of-range CRAM reads contribute zero. `0` means no signature. The signature
is computed only after the palette line remains unchanged for 30 frames
(`SIG_STABLE_FRAMES`), which prevents fades from fragmenting identity.

Variant resolution minimizes total cost; ties select the lower variant index:

| Dimension | Match | Wildcard | Mismatch |
|---|---:|---:|---:|
| Palette | 0 | 20 | 200 |
| H flip | 0 | 1 | 2 |
| V flip | 0 | 1 | 2 |
| Palette signature | 0 | 50 | 400 |

## Plane-tile hash

Reference: plane observation in `src/ayther_session.cpp` and the independent
oracle in `tests/plane_hash_variants_test.cpp`.

Read a nametable word with the word-swap correction:

```text
rd(off) = vram[off ^ 1]
word    = (rd(base + cell*2) << 8) | rd(base + cell*2 + 1)
pattern = word & 0x07FF
palette = (word >> 13) & 3
hflip   = (word >> 11) & 1
vflip   = (word >> 12) & 1
```

Pattern zero is an empty cell and is skipped. Otherwise:

```text
h = SEED
for b in 0 .. 32:
    h = mix(h, vram[(pattern * 32 + b) ^ 1])
h = mix(h, palette & 3)
```

This hash is flip-invariant but not palette-blind. The palette line is the
final mixed byte. `ayther_plane_tile_hash_repalette` and
`ayther_plane_tile_hash_variants` derive another palette variant by reversing
the final FNV step and applying the requested palette.

> [!IMPORTANT]
> Sprite graphics do not apply `^ 1` and ignore palette. Plane tiles apply
> `^ 1` and include palette. Confusing the two definitions is a common silent
> interoperability failure.

## Audio-event signature

Reference: `core/src/audio_event.rs`.

Input is the fork's raw chip-write stream `{cycle, addr, data, chip}`. `cycle`
does not participate, which keeps the identity stable during replay. Chip IDs
are `0 = FM/YM2612`, `1 = PSG/SN76489`, and `3 = PCM/RF5C164`. FM `addr` is the
latched register in `0x000..0x1FF`, not the bus port.

Event boundaries:

| Source | Opens | Closes |
|---|---|---|
| FM channels 0–5 | write `0x28` with `(data & 0xF0) != 0`; invalid channel code 3 is ignored; channel 5 is ignored while DAC is active | key-off; end is `max(frame-1, start)` |
| DAC, reported as FM channel 5 | first frame with DAC enabled, writes to `0x2A`, and sample range at least 8 | after six silent frames; end is the last frame carrying signal |
| PSG channels 0–3 | attenuation differs from `0x0F` | attenuation becomes `0x0F` |
| PCM channels 0–7 | typed key-on; retrigger closes then opens | typed key-off; pitch and volume do not alter the captured signature |

A key-off without a key-on produces a residual event `[0, frame-1]` only when
`frame > 0` and the corresponding bit was present in `initial_active`.

Signatures use the AYTHER FNV seed:

```text
FM:  h = mix(mix(SEED, 0), channel)
     mix registers base 0x30..0x9C step 4, then 0xA0,0xA4,0xB0,0xB4
     bank = channel < 3 ? 0 : 0x100; index = channel % 3

DAC: h = mix(mix(mix(SEED, 0), 5), 0xDA)
     mix each ordered 0x2A sample byte from the opening block

PSG: h = mix(mix(SEED, 1), channel)
     mix frequency low byte, then high byte

PCM: h = mix(mix(SEED, 3), channel)
     mix start, loop-start low/high, and frequency-delta low/high
```

The channel mask returned by `ayther_chan_bit(chip, channel)` assigns FM to
bits 0–5, PSG to 6–9, and PCM to 10–17. `kAytherAllChannels` is `0x3FFFF`;
bits 18 and above are rejected.

## Instrument identity

Instrument identity excludes note, channel, pan, and effective volume.

- FM uses namespace byte `0xF1`, the full patch, feedback/algorithm, and
  AMS/FMS. It masks pan from `B4` and omits total-level registers for carrier
  operators selected by
  `FM_CARRIERS = [0x8,0x8,0x8,0x8,0xA,0xE,0xE,0xF]`.
- PSG tone channels share namespace identity `mix(mix(SEED,1),0xF0)`; the
  noise channel additionally mixes its noise-mode nibble.
- DAC instrument identity equals the event signature because attack PCM bytes
  identify the sample.
- PCM uses namespace `0xF2` plus start and loop-start addresses, excluding
  channel and frequency delta.

Velocity and MIDI pitch are derived presentation data, not identity. Current
derivations live in `audio_event.rs`; changing them may alter playback but must
not silently alter an identity key.

## Screen-plane signature

Each observed non-empty cell contributes:

```text
x  = cell_hash
x ^= column     * 0x9E3779B97F4A7C15
x ^= row        * 0xC2B2AE3D27D4EB4F
x ^= (plane+1)  * 0x165667B19E3779F9
x ^= x >> 33
x *= 0xFF51AFD7ED558CCD
x ^= x >> 29
sig_plane[plane] += x  # wrapping, commutative sum
```

Observation uses `column = screen_x >> 3` and `row = screen_y >> 3`, providing
sub-cell phase invariance. Signatures remain separate per plane. Recognition
requires exact signature and declared-cell count for every selected plane;
entry hysteresis is two frames and exit is immediate. Presence gates are the
alternative mechanism and open at 60 percent matching hashes per layer.

## Spatial field units

| Field | Unit and coordinate space |
|---|---|
| Picture `cells` | Unsigned 8-pixel screen cells, top-left origin |
| Panorama `cells` | Signed 8-pixel level-space cells; origin is the minimum observed coordinate |
| Set `tiles` | Signed cell offsets relative to the set anchor |
| Pose `rel` | Signed screen pixels relative to pose origin |
| Pose `dims` | Pixel dimensions for each member |
| `flips` | Bit 0 H flip, bit 1 V flip |
| `slots` | `u16` mask; slot values must be below 16 |
| `ref` / `refs` | Average capture-time RGB in 0–255; layered form prefixes palette line 0–3 |

## Accepted replacement asset contracts

These formats are presentation contracts rather than identity inputs, but pack
tooling must agree with the Engine about them:

| Asset | Contract |
|---|---|
| Image | PNG with alpha; alpha defines the HD cutout; decoded to RGBA |
| Audio | WAV, OGG/Vorbis, or FLAC; converted once per asset to interleaved S16LE stereo at 44.1 kHz |
| SoundFont | Flat SF2 in the pack; SF3 and SFZ are normalized to SF2 before packaging |
| Video | `.ivf` containing VP9 all-keyframes only; VP8 is not accepted |

Media decoders still require independent decoded-size and processing limits.
Format acceptance does not establish that the pack author owns the content.

## Obtaining identities without AYTHER Lab

Community tooling can obtain the same identities through the public Engine/core
boundary:

1. For sprites, read parsed-sprite regions `0x10B`/`0x10C` plus VRAM and call
   `ayther_sprite_hasher_process_sprites`. Occurrences return hash, animation
   group, palette, flips, and slot. With a stock core,
   `ayther_sprite_hasher_process_vram(..., AYTHER_SAT_AUTODETECT)` searches SAT
   candidates aligned to `0x200`, requires at least three link hops and three
   on-screen sprites, scores `on_screen * 1000 + min(hops, 999)`, and chooses
   the lowest base on a tie.
2. For plane tiles, implement the plane-tile definition above over VRAM and VDP
   registers, or validate candidates through `ayther_tile_sub_lookup`.
3. For audio, feed regions `0x109`/`0x10A` into
   `ayther_audio_event_process_frame`, then inspect closed and active events.
4. For palette variants, call `ayther_palette_signature` over CRAM region
   `0x100` and preserve the 30-frame stability latch.
5. For stable asset names, use `ayther_asset_id` or
   `ayther_asset_id_bytes` rather than inventing another normalization.

The AYTHER-aware fork additionally exposes VDP registers at `0x101`, VSRAM at
`0x107`, and Z80 RAM at `0x10F`. Those observations are unavailable from a stock
core unless the standard libretro surface provides an equivalent. The
versioned integration rules are in
[Emulator extension ABI](EMULATOR_EXTENSION_ABI.md).

Pack-building, signing, conformance fixtures, and authoring UI belong to
separate SDK or product artifacts. Their absence from this repository does not
make the identity contract proprietary.

## Known-answer tests

`core/src/identity_kat.rs` exposes pure implementations and hand-recorded
expected values. The synthetic VRAM pattern is
`vram[i] = (i * 37 + 11) & 0xFF`; 37 is coprime with 256, so adjacent swapped
bytes differ and the fixture actually detects a missing or extra `^ 1`.

Published anchors include:

| Identity | Expected value |
|---|---:|
| Sprite, tile 0, 1×1 | `0x41573b4753eaaeb4` |
| Sprite, tile 3, 4×4 | `0xadba9fc9e8c721a7` |
| Plane tile, pattern 1, palette 0 | `0x98d36be2cd04adb9` |
| FM event over the synthetic register bank, channel 0 | `0xab387bbb7090249b` |

The KAT suite also checks equivalence with the production `SpriteHasher`,
palette sensitivity differences, zero-fill behavior, the exact FNV prime in
decimal, and an independently written plane-tile calculation.

## Reimplementation hazards

The following mistakes compile and run but produce incompatible packs:

1. using the canonical FNV offset basis;
2. applying word-swap correction to sprites or omitting it from plane tiles;
3. applying H/V flips while decoding identity graphics;
4. sorting `pose_key` members or failing to sort `anim_group_id` members;
5. including FM carrier total level in instrument identity;
6. including audio-write cycle in an event signature;
7. combining screen signatures across planes.

An independent implementation should run the published KATs before consuming
real content. ROM-based comparison is useful integration evidence but cannot
replace redistribution-safe synthetic vectors.
