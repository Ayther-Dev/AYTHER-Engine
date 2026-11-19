# Frames, staging and delivery probe

Integrated in the [conversion probe](../conversion_probe/README.md) CMake
project; it reuses the rc.9 SDK and the observational SDL copy. It compares the
published `AudioPlayer` library with methods extracted verbatim from the
reference and compiled under another class name with state observers, so SDK
symbols are not replaced. Layout, rules, algorithms and method conditions are
unchanged. Shared conventions are in the [audio QA overview](../README.md).

Configuration requires the hashes of `audio_player.cpp` and `audio_player.h`
and extracts initialization, staging, flush and cleanup; no decoders are needed
for the synthetic PCM inputs. The observed copy records `mark_frame_boundary`,
each batch, router add or replace, state before and after mute and mixer, and
discards. SDL callbacks keep successful submissions, conversion and the real
postmix.

## Staging cases

Two controlled frames provide three batches: hash 11 in `[0, 32)`, hash 12 in
`[32, 48)` and hash 22 in `[48, 96)`. Cases: `batches`, `mute`, `add`,
`replace`, `discard`, `suppress` and `lost`. The full state table is verified,
not only the result. `lost` omits the `post_mix` state and must return 2
(`staging_trace_incomplete`) with audio intact.

Each process keeps `input.s16le`, `mix.f32le` and `staging.toml`. The manifest
includes PCM of intermediate states, batches, controlled-frame identity, local
mark, main timeline, the link between each submission and the observed state,
and conversion spans with Q32 phase. Inputs are S16LE stereo at 44100 Hz and
outputs F32LE stereo at 44100 Hz. `states_before` of each submission tells how
many states had occurred when SDL accepted its bytes.

The first delivery is the real 3072-frame priming, which is not part of the
mixer timeline. The second matches the `post_mix` state. EOS drains what was
already produced; no frames run and no PCM is generated during close. The
conversion record is compared exactly with the real postmix output.

## HD voice

`voice`, `voice_suppress` and `voice_lost` use `play_event_hd` and the
synthetic pack of the conversion probe, with its explicit trust for this
invocation only. In controlled frame 1 the mixer makes 48 contributions, from
timeline 48 to 96, matching output `[3120, 3168)`. Suppressing the original
keeps the HD. The control drops only the timeline-60 record and returns 2 with
identical audio.

Start, WAV loading and conversion methods are extracted from the reference;
`stb_vorbis` and `dr_flac` declarations are included and the decoders come
from the SDK. `QA_DECODER_INCLUDE` names an existing local prefix for
`dr_flac.h`, validated by hash; it defaults to
`build/windows-native/vcpkg_installed/x64-windows/include`. Asset preparation
uses a temporary converter and finishes before gameplay submissions; track
positions are frames of the converted PCM.

## Resume reason and effect

`resume_voice` and `resume_lost` consume the state and input of the
[reference oracle](../reference_oracle/README.md) by hash, call
`live_resume_decide`, and apply its offset through `play_event_hd` with the
original end and cut. The observed copy records a label immediately before
`return Restart`, without changing the computation. The 48 contributions start
at cursor 614 of the converted track and appear at output `[3120, 3168)`. The
decision's logical frame 120 and the staging's controlled frame 1 are separate
domains. `resume_lost` drops only the reason and returns 2 even though audio,
voice and positions match.

## Run

After configuring the conversion probe with `QA_ENGINE_REFERENCE_SOURCE`,
`Ayther_DIR` and `CMAKE_PREFIX_PATH`:

```powershell
cmake --build build/qa-conversion --config Release --parallel 4
ctest --test-dir build/qa-conversion -C Release -R audio_qa_staging --output-on-failure
```

Limits: two effective submissions, up to four storable submissions, 32 states
of at most 128 frames and four batches each, 32 converted spans, 4096 input and
output frames, up to 96 mixer contributions. DRC is not disabled and published
packages are not modified. This proves the controlled `AudioPlayer` boundary,
not frames from a ROM or take replay.
