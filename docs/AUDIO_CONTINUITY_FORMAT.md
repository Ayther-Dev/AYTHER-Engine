# Audio continuity pack format

**Status:** implemented; schema `1`

**Last verified:** 2026-11-19

Continuity says how an HD audio replacement behaves when its trigger repeats,
ends or changes: whether it continues, restarts or overlaps, how it
transitions, and which bus it competes on. It is stored on each `[[event]]`
entry of `audio_events.toml`, next to the event's existing `signature`,
`asset`, `duration`, `loop` and related fields. The format is the same for
packs played on the `libretro` and `tessera_native` providers.

## Schema 1 fields

`continuity_schema = 1` opts an event into the explicit contract. Every field
below is optional; an omitted field takes the default of the event's
`category`.

| Field | Type and unit | Valid values |
|---|---|---|
| `category` | string | `music`, `ambient`, `effect` (default), `voice` |
| `repeat_policy` | string | `continue`, `restart`, `overlap` |
| `transition_policy` | string | `cut`, `fade` |
| `loop_begin`, `loop_end` | source sample frames | both or neither; `0 <= begin < end <= 4294967295` |
| `entry_point` | source sample frames | non-negative |
| `fade_in_duration_ns`, `fade_out_duration_ns` | nanoseconds | 0 through 600,000,000,000; non-zero requires `transition_policy = "fade"` |
| `gain_linear` | finite float | 0 through 4 |
| `sequence_priority`, `bus_priority` | signed integer | int32 range |
| `max_voices` | unsigned integer | 1 through 256; must be 1 when `exclusive_bus` is true |
| `exclusive_bus` | boolean | strict TOML boolean |
| `bus` | UTF-8 identifier | 1 through 64 bytes; defaults to the category name (`effects` for `effect`) |

Category defaults (`default_audio_playback_policy`,
`include/ayther/audio_playback_policy.h`):

| Category | Repeat | Transition | Voices | Exclusive bus |
|---|---|---|---:|---|
| `music`, `ambient` | `continue` | `cut` | 1 | yes |
| `effect` | `overlap` | `cut` | 32 | no |
| `voice` | `restart` | `cut` | 8 | no |

## Legacy events

An event without `continuity_schema` is a legacy event. The reader sets
`used_legacy_defaults` and derives the category from the event's `bus` key:
`music` and `voice` map to those categories, anything else (including an
absent key) maps to `effect` on bus `effects`. The policy is then the category
default above.

## Validation

- Values keep their declared units; the Engine does not clamp or silently
  discard invalid values.
- Any invalid value rejects the whole continuity catalogue: `ok` is false,
  `values` is empty, and `diagnostic` names the event signature, the field, the
  actual value and the expected range.
- A `continuity_schema` other than 1 rejects the whole catalogue.
- When the catalogue is rejected, `AytherSession` logs the diagnostic under
  `audio_continuity_schema` and loads none of the file's audio assignments.
- Entries that are not tables or have no non-zero `signature` are skipped.
- Reading never rewrites the pack.

## API

The installed package exposes the reader and value types through
`<ayther/ayther_components_toml.h>`:

```cpp
ayther::AudioContinuityCatalog result =
    ayther::parse_audio_continuity_toml(audio_events_text);
if (result.ok) {
  // result.values: signature -> AudioContinuityConfig
}
```

Check `result.ok` before using `result.values`. This schema is independent of
the `.ay` container format and the manifest schema.
