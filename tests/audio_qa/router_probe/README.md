# Voice-router resampler probe

Isolated probe of the voice router's `StreamResampler` and the Engine
consume/zero-fill boundary of the `v0.1.0-rc.9` reference. Configuration
checks the hashes of the two implementation files and the header, extracts
their sections verbatim, and generates reference and observed variants in the
build tree; the filter is not rewritten. Shared conventions are in the
[audio QA overview](../README.md).

The session section keeps its original decisions: reset on lag, zero
initialization, pull and deficit counter. The harness supplies only the members
and inputs that section uses. The observed variant records phase before and
after, effective step, history size, indices actually read, produced values,
clips and resets, and keeps the zero ranges of each request the resampler did
not produce. SoundFont summing and staging are out of scope (see the
[staging probe](../staging_probe/README.md)).

Synthetic input at 53267 Hz, output at 44100 Hz then 48000 Hz with phase
preserved. Seven requests produce 328 output frames: 204 computed by the
filter and 124 zeros from the consume limit. A real queue excess triggers the
original reset, discarding `[142, 416)` of the absolute input domain
(including retained history); the next input starts at 416 and no continuity
is attributed to discarded material.

```powershell
cmake -S tests/audio_qa/router_probe -B build/qa-router `
  -G 'Visual Studio 18 2026' -A x64 -T v145 `
  '-DQA_ENGINE_REFERENCE_SOURCE=<qa-reference>/engine-source'
cmake --build build/qa-router --config Release --parallel 4
ctest --test-dir build/qa-router -C Release --output-on-failure
```

All three cases require byte equality with the reference. `lost` drops the
record of the reset with queue; `phase` alters one copied phase value. Neither
alters PCM; both must return 2 (`router_trace_incomplete`). Fields derived from
an incomplete record are not presented as verified positions.

Limits: 512 input frames, 328 output frames, seven blocks, up to 1024 records.
Phase is stored as a round-trip-exact double in TOML, as in the original.
