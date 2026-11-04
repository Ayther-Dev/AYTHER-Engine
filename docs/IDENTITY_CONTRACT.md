# Identity contract v1

**Status:** draft for approval (spec 008, TP-011.1); version 1 is what gpgx `ayther-abi-1.10-r4` produces with the algorithms below.

This table names every identity class, how it is computed, from which observables, who computes it and which emulator timing behaviour an emulator core must reproduce so the identifier does not change. The algorithms themselves are specified in [Pack identity specification](IDENTITY_SPECIFICATION.md). A core that changes any identifier is a pack-compatibility change, not a core detail.

Changing this contract requires a new version of the contract and a new spec. Packs record the version they were made with (`[identity] contract`); a reader that finds a greater version rejects the pack with `pack.identity_contract_too_new`.

| Class | Identifier | Input observables | Computed by | Timing sensitivity the core must reproduce |
|---|---|---|---|---|
| `sprite` | xxh3_64 over the pattern nibbles, host order, algorithm 1 | VRAM, `PARSED_SPRITES` (pattern, width, height) | Engine | Which sprites are parsed and drawn per line (`SPRITE_OUTCOME`: `DROP_LINE`, `DROP_PIXEL`, `MASKED_X0`) and the order of the sprite attribute table |
| `plane_tile` / `window_tile` | FNV-1a-64 over 32 bytes in bus order plus palette, algorithm 1 | VRAM, `LINE_CELLS`, `ATTRIBUTION` | Engine | Per-pixel attribution and per-line register changes (`LINE_REGS`, `LINE_CRAM`) |
| `pose` | Canonical multiset of (x, y, hash) over the four mirrors | Sprites of the frame | Engine / Lab | Capture order (sprite attribute table) |
| `palette_variant` | xxh3 of the 16-colour line after 30 stable frames | CRAM, `LINE_CRAM` | Engine | Per-frame stability |
| `audio_event` (`fm`, `psg`, `dac`, `pcm`) | FNV over registers, FM without carrier TL, algorithm 3 | `AUDIO_WRITES` (order and values), event limits at key-on and key-off | Engine | Order of the writes within the frame; the value of `cycle` is not used |
| `instrument` | Space `0xF2` over start and loop (PCM) or the FM patch | Audio events | Engine | None |
| `element` (Drawing / Object) | Set of `plane_tile` / `sprite` with layout | Lab catalogues | Lab | None |
| `panorama` | Commutative sum per plane and cells | `LINE_CELLS`, per-line scroll | Engine / Lab | Per-line scroll and raster bands |
| `sequence` | Signature of the FM/PSG event sequence | Audio events | Lab (detector) | Segmentation by frame |

## Time base of `cycle`

Pending (TP-011.2): it is fixed with a synthetic probe ROM measured against gpgx. No identity depends on its value; only the D-3 equivalence does.

## Versioning

| Version | Meaning |
|---|---|
| 1 | What gpgx `ayther-abi-1.10-r4` (`7109a7c2`) produces with the algorithms above |
