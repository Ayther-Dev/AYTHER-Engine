# Audio QA probes

Probes and oracles for the Engine audio path: the audio player, the HD mixer,
SDL conversion and the decision code behind HD audio replacement. They support
the [audio observation contract](../../docs/AUDIO_OBSERVATION.md). None of them
runs an emulator, so they are independent of the emulation provider.

| Directory | Covers |
|---|---|
| (this directory) | main-output capture through the real `AudioPlayer` and device postmix |
| [`observer_contract/`](observer_contract) | the observation producers, queues, HD-state restore and end-of-replay operations; part of the native CTest |
| [`conversion_probe/`](conversion_probe/README.md) | SDL converter phase, common mix, drain, reset and priming |
| [`staging_probe/`](staging_probe/README.md) | frames, staging and delivery in `AudioPlayer` |
| [`router_probe/`](router_probe/README.md) | voice-router resampler |
| [`voice_probe/`](voice_probe/README.md) | per-occurrence mixer observation |
| [`continuity_oracle/`](continuity_oracle/README.md) | live edge gate and repeated triggers |
| [`lifecycle_oracle/`](lifecycle_oracle/README.md) | mixer cursor, loop, window and fade lifecycle |
| [`selection_oracle/`](selection_oracle/README.md) | assignment selection |
| [`reference_oracle/`](reference_oracle/README.md) | live-resume decision |

## Conventions shared by every probe

- **Reference build.** Standalone probes compare against a pinned reference:
  the Engine `v0.1.0-rc.9` source (`QA_ENGINE_REFERENCE_SOURCE`) and, where a
  published package is needed, its `ayther-engine-vpx` Windows package
  (`Ayther_DIR`) and the matching Runtime binaries (`QA_RUNTIME_BIN`).
  Configuration verifies the SHA-256 of every reference file it extracts and
  never downloads dependencies.
- **Observed copy.** Instrumented code is a private copy generated in the build
  tree from the reference, with hooks added and the original decisions left
  intact. Reference sources and production code are never modified.
- **Negative controls.** Each probe has a control that drops or corrupts only
  the observation record (never the PCM) and must fail with a specific exit
  code. A probe whose control passes is vacuous.
- **Bounded hooks.** Hooks copy into fixed arrays; they do not allocate, block
  or write files. Exhausting a bound invalidates the trace. Each process has a
  three-second limit. Results go to a new directory per test.
- **Units.** Positions count sample frames; ranges are half-open. No position
  is derived from wall-clock time.
- **Scope.** A probe proves its controlled boundary only. None replaces a replay
  of a real take or product acceptance.

Commands below use these placeholders:

| Placeholder | Meaning |
|---|---|
| `<qa-reference>` | local directory holding `engine-source`, `engine-package` and `runtime-package` for the pinned reference |
| `<vcpkg-root>` | vcpkg checkout providing `scripts/buildsystems/vcpkg.cmake` |
| `<runtime-vcpkg-installed>` | the `vcpkg_installed` prefix of the matching Runtime build |

## Main-output capture

The probe feeds the 1024-frame `known-pcm-v1` block (S16LE stereo, 44100 Hz,
SHA-256 `5d764a90b6ae1b6606aa95bbd343c54fd1a7b3aa7f7f20e116afc53423081602`,
`data/known-pcm-v1.s16le.pcm`) to `AudioPlayer::buffer_emulator`, runs
`flush_emulator`, and captures the postmix of the same logical device with
`SDL_SetAudioPostmixCallback`. It does not recompose a mix or replace
`AudioPlayer` with a double. Default options are explicit and DRC stays on.
`AudioPlayer` is internal to Engine and not installed in its SDK, so this probe
lives in Engine.

CTest pins an SDL dummy device at 44100 Hz, two channels and 512-frame
requests; the executable requires the dummy driver and validates the effective
format of every callback. The callback copies PCM and records consecutive
ranges in preallocated storage (up to 32768 frames and 256 blocks) and stops
after at least 16384 frames (1024 for the closing cases). Uninstalling the
callback synchronizes its completion before data is read.

Each run keeps `capture.f32le`, `capture.toml`, stdout and stderr. The manifest
lists every real block with its half-open range, the main timeline before and
after the flush, the observed DRC and hashes. The output origin is the first
retained callback.

Correspondence is accepted by searching the window with the lowest RMS error
against the normalized fixture, threshold 0.025. This tolerates the DRC
transform and does **not** prove per-sample alignment: `match_sample_begin` is
a diagnostic anchor, while callback ranges are exact. Manifests keep
`exact_effect_alignment_verified = false`.

| Mode / negative control | Check |
|---|---|
| `known` / `silence` | the fixture is found; zeros through the same path must be rejected (`known_input_not_found`) |
| `auxiliary` / `auxiliary_missing` | a 1024-frame F32 synth signal is located exactly once in the postmix |
| `prime` / `prime_missing` | 512 frames of silence inserted by `prime_synth` between two distinct marks |
| `conversion` / `conversion_missing` | a preview S16 converted to F32 at 44100 Hz equals the normalized conversion in all 2048 values |
| `closing` / `closing_discard` | a queued preview on an initially paused device drains exactly its 1024 frames at close |

Negative controls pass only when the executable returns the expected rejection
(3 for missing content, 4 for a missing exact auxiliary signal).

### Run against the pinned reference (Windows)

```powershell
cmake -S tests/audio_qa -B build/qa-main-capture -G 'Visual Studio 18 2026' -A x64 -T v145 `
  '-DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake' `
  '-DVCPKG_INSTALLED_DIR=<runtime-vcpkg-installed>' `
  -DVCPKG_MANIFEST_INSTALL=OFF `
  '-DAyther_DIR=<qa-reference>/engine-package/ayther-engine-vpx-v0.1.0-rc.9-windows-x86_64/lib/cmake/Ayther' `
  '-DQA_ENGINE_REFERENCE_SOURCE=<qa-reference>/engine-source' `
  '-DQA_RUNTIME_BIN=<qa-reference>/runtime-package/bin'
cmake --build build/qa-main-capture --config Release
ctest --test-dir build/qa-main-capture -C Release -R '^audio_qa_main_capture_' --output-on-failure
```

`QA_RUNTIME_BIN` copies the reference distribution's DLLs next to the
executable; evidence must record the SHA-256 of the `SDL3.dll` actually used,
not only its version.

The directory is also part of the native Engine CTest. There, build the
`audio_qa_main_capture` target and select `^audio_qa_main_capture_`; that run
uses the native build, not the rc.9 reference. Tests have timeouts and fail
CTest; they are never converted to skips.
