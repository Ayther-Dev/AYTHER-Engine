# SDL conversion, common mix and drain probes

Feasibility probes that observe the real phase of the SDL converter without
changing its output, and then the common mix, drain, reset, priming and
mid-consumption transitions of the audio player. They are separate from the
product API and are not a Runtime replay. Shared conventions are in the
[audio QA overview](../README.md).

## Instrumented SDL

Configuration checks the hashes of `SDL_audiocvt.c` and `SDL_audioresample.c`
in a local SDL 3.4.8 source tree (`QA_SDL_SOURCE`), copies the tree into the
build directory, and adds an observer after each successful conversion. The
original tree, the dependency cache and the published DLL are not modified; the
altered copy is identified as such and keeps the SDL license. Algorithm,
ratio, requested frame counts, gain and queue handling are unchanged.

The observer records format, frames consumed and produced, Q32 step, phase
before and after, padding and gain, and copies borrowed PCM into fixed storage.
It is registered before streams are created and removed after they are
destroyed; this lifetime is mandatory for the experimental contract, which has
no synchronization of its own. Isolated analysis of `observer_impl.h` also
needs `audio/SDL_audioqueue.h` preloaded; the original SDL unit already
includes it.

Deriving phase from `GetAudioStreamQueued`, `Available` or callback byte counts
was rejected: those do not observe the fractional phase or each internal
consumption, and changing or disabling DRC would not test the transform under
investigation. The modified DLL is not a product dependency.

## Converter phase

Two executables differ only in whether they require the observer: one loads
the published DLL and verifies the experimental symbol is absent, the other
loads the instrumented copy and requires it. Both convert the known PCM
fixture with identical operations, gain 0.75 and variable request sizes. Cases
cover S16 and F32 at 44100 Hz, 44100→48000 Hz, and an initial ratio of
1.000083327293396 followed by 1.0075 after the first consumption. Checks:
continuity and phase preservation, observed PCM equal to returned PCM, and byte
equality with the published reference. The post-phase is observed directly;
the phase equation only checks coherence.

The `drop` and `phase` controls alter only the copied observations (loss of the
first span, or one phase bit) and must fail integrity.

Limits: one 1024-frame input, up to 4096 output frames, 128 recorded
operations, three controlled formats or ratios, no device, no wall clock. EOF is
kept with `FlushAudioStream` and no other input is added. This does not prove
per-voice filter effects, resets between tracks, stream mixing or all formats,
nor binary identity between the published and rebuilt SDL.

```powershell
cmake -S tests/audio_qa/conversion_probe -B build/qa-conversion `
  -G 'Visual Studio 18 2026' -A x64 -T v145 `
  '-DQA_SDL_SOURCE=<sdl-3.4.8-source>' `
  '-DQA_RUNTIME_BIN=<qa-reference>/runtime-package/bin'
cmake --build build/qa-conversion --config Release --parallel 4
ctest --test-dir build/qa-conversion -C Release --output-on-failure
```

Each run keeps PCM, metadata, hashes and logs in a new directory. A missing
observer, a DLL other than the pinned reference, discontinuity, overflow or a
PCM difference fails the probe.

## Common mix

Configuring also `QA_ENGINE_REFERENCE_SOURCE`, `Ayther_DIR` of the rc.9
reference and `CMAKE_PREFIX_PATH` (the Runtime vcpkg prefix used by the
main-output capture) adds `audio_qa_common_mix` with cases `all`, `gain`,
`muted`, `main_only` and `missing_contribution`.

The published SDK runs `AudioPlayer`; the private DLL observes submissions and
conversions. Five families are identified: main, synth, PCM preview, asset
preview and hash substitutions; the main family also has a real HD voice.
Submissions are recorded under an initial pause and capture is published with
an atomic flag on resume. During observation only the audio thread modifies
the buffers, and the reader waits for it, pauses the device and uninstalls the
postmix before reading.

Outputs: `mix.f32le` (real postmix), `contributions.f32le` (diagnostic copies)
and `mix.toml` with ranges, phases and gains. Contribution ranges are located
by consumption order within the logical callback, and a sum clamped to
`[-1, 1]` after each contribution checks correspondence without altering real
audio. `missing_contribution` drops one contribution from the record and fails.
`muted` still requires consumption and positions with zero final PCM;
`main_only` keeps the inactive auxiliaries explicitly.

Limits: 8192 final frames, five paths, 256 spans and 40960 contribution frames.
Without EOS the main stream leaves 7 frames pending in the resampler in this
scenario, so it is not presented as a complete close.

The pack in `data/` holds only the synthetic PCM fixture; its trust registry
is used only for this invocation. `generate_fixture.py` regenerates it with
Python and `cryptography` 50.0.1 using a deterministic test public key that must
never enter a product trust store. CTest verifies the pack and registry hashes.

## Drain with frozen generation

`drain` and `drain_loss` fix the available output limit after EOS is declared
on the five existing queues with the device paused, with no further feed or
mixer advance; an atomic counter requires zero new submissions after the
freeze. The positive case keeps the 4096 main frames, including the seven held
before EOS, and drains the other queues. The capture limit is the maximum of
those limits within the 8192-frame buffer; a last callback past the limit keeps
only its applicable prefix. The control clears the main queue after the limit
is fixed and returns 4 (`drain_incomplete`). The HD voice may remain active
because its future portion is not synthesized.

## Reset with active phase

`reset` consumes 97 converted frames, clears the stream, and feeds it again
with the fixture values in reverse interleaved order. The observer keeps queue
and phase before and after `ClearAudioStream`. Input spans use a different
absolute origin after the clear, while retained output stays contiguous.
Output is compared with the published DLL. `reset_drop` and `reset_phase` alter
only the record and must fail.

## Priming after resampling

`prime_resample`, `prime_missing` and `prime_loss` first open a dummy device at
48000 Hz; `AudioPlayer` uses its normal initialization and the probe verifies
the effective format (the frequency environment variable alone does not change
the backend default). 512 real `prime_synth` frames are inserted between two
1024-frame signals at 44100 Hz and the five families drain without new audio.

Submission records check the real bytes of each chunk (at most 16 chunks of up
to 65536 bytes). The SDL copy exposes the compiled filter support and each
conversion records it with the real phase. Input silence `[1024, 1536)` maps to
output support `[1109, 1678)`; the pure span `[1120, 1666)` must be zero.
`prime_missing` omits the insertion and `prime_loss` drops one conversion
record without altering PCM; both return 5 (`prime_alignment_incomplete`).
Candidate boundaries in incomplete results do not certify silence.

## Changes during consumption

`gain_switch`, `mute_switch`, `transport_cut`, `previews_cut` and `natural_end`
observe the five families before and after output boundary 512. The harness
pauses at that boundary, applies the real API and resumes, without feeding more
data. `ClearAudioStream` and `DestroyAudioStream` report real queue and phase,
and the record distinguishes that instant from each path's last converted
contribution. Cut paths leave an explicit discarded queue; the others must
finish consuming. `gain_switch_loss` drops the boundary record and returns 6
(`transition_trace_incomplete`). This is not a public API or a physical
latency measurement.
