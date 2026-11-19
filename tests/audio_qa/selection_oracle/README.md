# Assignment selection oracle

Compiles `AudioMatchIndex` and the `resolve_event_sig` method extracted
verbatim from the pinned Engine `v0.1.0-rc.9` session. The harness supplies the
catalogue and the index the method consults. The observed version copies
candidates and results inside the original branches; the reference has no
hooks. Both receive the same state and synthetic event. Shared conventions are
in the [audio QA overview](../README.md).

The catalogue holds one exact assignment (99) and three instrument/pitch rules
for instrument 7: 10/63, 20/64 and 15/64.

| Case | Expected |
|---|---|
| `match` | pitch 64 selects 15 through the real tie-break |
| `miss` | pitch 62 keeps three rejections and no match |
| `exact` | signature 99 is a direct selection; no broad candidate is visited |
| `lost` | candidate 20 is removed from the record only; returns 2 with the same decision as `match` |

State, input, decision and facts are written separately. The unmodified output
sentinel is kept as a decimal string, never equated with a selected
assignment. The event comes from the fixture, not from a detector or ROM. No
audio starts.

```powershell
cmake -S tests/audio_qa/selection_oracle -B build/qa-selection `
  -G 'Visual Studio 18 2026' -A x64 -T v145 `
  '-DQA_ENGINE_REFERENCE_SOURCE=<qa-reference>/engine-source'
cmake --build build/qa-selection --config Release --parallel 4
ctest --test-dir build/qa-selection -C Release --output-on-failure
```

Limits: one event per process, three broad rules and one exact, 32 facts.
Output is written after the decision, to a new directory.
