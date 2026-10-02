# Audio continuity pack format

Status: implemented. Capability `audio_continuity`, schema `1`.

Continuity is stored on each `[[event]]` entry in `audio_events.toml`. The
event keeps its existing `signature`, `asset`, `duration`, `loop` and related
fields. `continuity_schema = 1` opts that event into the explicit contract.

| Field | Type and unit | Valid values |
|---|---|---|
| `category` | string | `music`, `ambient`, `effect`, `voice` |
| `repeat_policy` | string | `continue`, `restart`, `overlap` |
| `transition_policy` | string | `cut`, `fade` |
| `loop_begin`, `loop_end` | source sample frames | complete pair, `0 <= begin < end <= uint32 max` |
| `entry_point` | source sample frames | non-negative and inside the decoded source when its length is known |
| `fade_in_duration_ns`, `fade_out_duration_ns` | nanoseconds | 0 through 600,000,000,000; non-zero requires `fade` |
| `gain_linear` | finite float | 0 through 4 |
| `sequence_priority`, `bus_priority` | signed integer | int32 range |
| `max_voices` | unsigned integer | 1 through 256; must be 1 for an exclusive bus |
| `exclusive_bus` | boolean | strict TOML boolean |
| `bus` | UTF-8 identifier | 1 through 64 bytes |

Missing `continuity_schema` means a legacy event. Engine reports that legacy
defaults were used and derives the policy from the old `bus` classification:
music continues exclusively with one voice; voice restarts with eight voices;
effects overlap with 32 voices. Transitions default to `cut`.

Schema 1 fields retain their declared units; Engine does not clamp or silently
discard invalid values. It rejects the continuity catalogue with a diagnostic
containing the logical signature, field, actual value and expected range.
Unknown schemas are rejected. The original pack is never rewritten during
reading or migration.

The installed C++ package exposes the reader and value types through
`<ayther/ayther_components_toml.h>`:

```cpp
ayther::AudioContinuityCatalog result =
    ayther::parse_audio_continuity_toml(audio_events_text);
```

`result.ok` must be checked before using `result.values`. This schema is
independent of the `.ay` container format and manifest schema.
