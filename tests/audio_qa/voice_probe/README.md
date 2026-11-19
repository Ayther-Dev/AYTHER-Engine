# Per-occurrence mixer probe

Isolated probe of `HdMixer` from the Engine `v0.1.0-rc.9` reference.
Configuration requires the SHA-256 of the reference header and generates a
private copy with an observational identity per voice and a callback after
each real contribution. The aggregate `started` counter and mixing decisions
are unchanged. Shared conventions are in the [audio QA overview](../README.md).

Each record keeps cursor before, index actually read, cursor after, effective
gain, remaining fade and mix values before and after. Records go to a
4096-entry array. Per-sample instrumentation exists only in this bounded probe.

Two executables run the same input sequence, one with the original header and
one with the generated copy. CTest requires 4096 identical bytes of S16 stereo
PCM at 44100 Hz and two distinct results: complete trace, and diagnosed loss.
The negative control drops the contribution of occurrence 1 at sample 80 and
must return 2.

Scenario, four occurrences:

| Occurrence | Range |
|---|---|
| loop with region `[3, 11)` | `[13, 768)` |
| muted voice consuming 14 frames | `[0, 14)` |
| retrigger of the first key while the earlier one fades | `[210, 220)` |
| eight-frame fade at window end | `[300, 308)` |

From 768 only original PCM remains after `cut_all`. Positions belong to the
main mix timeline, not to the SDL output domain.

```powershell
cmake -S tests/audio_qa/voice_probe -B build/qa-voice `
  -G 'Visual Studio 18 2026' -A x64 -T v145 `
  '-DQA_ENGINE_REFERENCE_SOURCE=<qa-reference>/engine-source'
cmake --build build/qa-voice --config Release --parallel 4
ctest --test-dir build/qa-voice -C Release --output-on-failure
```

Limits: 1024 output frames, four occurrences, 4096 records. The per-occurrence
identity exists only in the experimental copy; the public equivalent is
`hd_mix_participant` in the [audio observation contract](../../../docs/AUDIO_OBSERVATION.md).
