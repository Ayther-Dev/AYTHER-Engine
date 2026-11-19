# Continuity and repeated-trigger oracle

Checks the live event edge gate, `LiveInstance`, `hd_fired` and the gameplay
branch of `play_oneshot_asset_file`, extracted from the Engine `v0.1.0-rc.9`
reference. The decisions and the call to `HdMixer::start` are the original
code. The reference mixer is compiled unchanged; the observed copy adds an
identity and a sample record. Shared conventions are in the
[audio QA overview](../README.md).

**Adapter boundary:** an event already selected, asset ready, no window, audio
and transport active, no bypass. A data provider supplies immutable decoded
PCM: 128 S16 stereo frames at 44100 Hz, sample value `1000 + 37 × index`.
Decoding and the detector are not run. The preview branch and window opening
reject use in this fixture instead of simulating it.

| Case | Expected |
|---|---|
| `keep` | the event stays active for four frames of 16 samples: one trigger, continuous cursor 0→64 |
| `repeat` | activity `[yes, yes, no, yes]`: the edge in frame 3 creates a new occurrence with the same key at 0→16; the previous one continues 48→64 under the original fade |
| `lost` | only the gate record of frame 3 is dropped; returns 2 with state and audio unchanged |
| `internal` | a sequence assignment with duration, ready asset and an empty anchoring result: the internal rejection creates neither a `LiveInstance` nor a voice |

```powershell
cmake -S tests/audio_qa/continuity_oracle -B build/qa-continuity `
  -G 'Visual Studio 18 2026' -A x64 -T v145 `
  '-DQA_ENGINE_REFERENCE_SOURCE=<qa-reference>/engine-source'
cmake --build build/qa-continuity --config Release --parallel 4
ctest --test-dir build/qa-continuity -C Release --output-on-failure
```

Limits: four frames, 64 mix frames, two occurrences, eight decisions and 128
sample records. The real contribution order is kept. State, inputs and PCM are
compared with the reference, and positions and gains are verified. This does
not cover a full session, SDL output or windowed sequence logic.
