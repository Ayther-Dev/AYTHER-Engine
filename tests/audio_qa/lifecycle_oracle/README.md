# Mixer lifecycle oracles

The Engine `v0.1.0-rc.9` mixer is compiled unchanged as the reference and as a
copy with hooks for the cursor, loop crossings, window removal and voice ends.
The adapter does not recompute any decision. Shared conventions are in the
[audio QA overview](../README.md).

Base fixture: synthetic eight-frame PCM with loop region `[2, 6)`, offset 4 and
gain 0.5, added to a constant original of 100. Results belong to the mixer's
main timeline, not to an SDL capture or a real take.

| Case / control | Expected |
|---|---|
| `loop` | six crossings 6→2 at samples 2, 6, 10, 14, 18, 22 |
| `window` | the voice stays in frame 2 and is removed in frame 3 by `end_frame = 2`, at mix boundary 20, final position 4, no later contribution |
| `lost` | only the first crossing record is dropped; identical audio, returns 2 |
| `replacement` / `replacement_lost` | two starts with the same key overlap for 56 frames; the second starts the fade of the earlier voice. Identities, cursor, effective gain per contribution and natural ends at 64 and 72 are kept, and the per-voice visit order is preserved even when sample bounds are not increasing. The control drops one end record and returns 2. |
| `natural` / `natural_lost` | constant original 100, eight-frame asset, gain 0.5, start 4, 16-frame mix: `[4, 12)` holds original plus HD, the rest original only; the natural end keeps position 8 and boundary 12. Dropping that end invalidates the trace with identical PCM. |
| `simultaneous` / `simultaneous_lost` | two voices share `end_frame = 1` and `cut_frame = 1`; only the first has an authored fade. The frame-2 tick visits voices 2→1: it cuts the second and starts the first's fade, which ends four samples later. Order is never reconstructed from timestamp proximity. A buffer of 32 tick contexts is kept; the control drops one cut. |

At the end of a run, cursor extremes indicate the retained final position, not
a new active playback.

```powershell
cmake -S tests/audio_qa/lifecycle_oracle -B build/qa-lifecycle `
  -G 'Visual Studio 18 2026' -A x64 -T v145 `
  '-DQA_ENGINE_REFERENCE_SOURCE=<qa-reference>/engine-source'
cmake --build build/qa-lifecycle --config Release --parallel 4
ctest --test-dir build/qa-lifecycle -C Release --output-on-failure
```

Limits: four frames, up to 72 mix frames, two occurrences, 32 events and 256
contributions. Saturation or loss invalidates the record. State and PCM are
compared with the reference.
