# Live-resume reference decision

Calls `live_resume_decide` directly from the pinned Engine `v0.1.0-rc.9`
header. It does not copy the algorithm or introduce an inferred reason. Shared
conventions are in the [audio QA overview](../README.md).

The case comes from `state.json` and `inputs.json` of the oracle fixture
(`QA_ORACLE_FIXTURE`). CTest verifies their hashes and passes arguments
separately, without shell concatenation. The `uint64` limit is kept as decimal
JSON text so it is never rounded through a floating-point number.

The looping instance starts at 30, ends at 180 and has cut `UINT64_MAX`; resume
is requested at frame 120, 60 FPS, known duration 3 s. The original branch
returns `restart` with offset 1.5 s. This is a logical decision; it does not
prove that the player was actually recreated, and the internal reason is kept
as not observed (the [staging probe](../staging_probe/README.md) observes it).

```powershell
cmake -S tests/audio_qa/reference_oracle -B build/qa-reference `
  -G 'Visual Studio 18 2026' -A x64 -T v145 `
  '-DQA_ENGINE_REFERENCE_SOURCE=<qa-reference>/engine-source' `
  '-DQA_ORACLE_FIXTURE=<oracle-fixture-directory>'
cmake --build build/qa-reference --config Release --parallel 4
ctest --test-dir build/qa-reference -C Release --output-on-failure
```

Two tests: the fixture decision, and rejection of a numeric input with a suffix
before any result is created. Limits: one instance, one operation, seven scalar
values. The result is written to a new directory and never replaces an existing
file. No SDL, core, ROM, network or replay is needed.
