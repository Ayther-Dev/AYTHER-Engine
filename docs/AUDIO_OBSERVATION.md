# Audio observation

**Status:** implemented; observation contract 1.0; HD audio state 1.1; `libretro` provider only

**Last verified:** 2026-11-19

The audio observation lets a QA host see, as typed facts and PCM blocks, what
the Engine's audio path did in a session: which assignments a pack declared
and loaded, what the detector received and produced, which assignment and
sequence candidates were considered and chosen, how each HD voice started,
advanced, looped and ended, what reached the mix, and what left the device.
It also defines the HD audio state a host saves and restores next to a
checkpoint.

The Engine produces facts; it does not serialize them, write evidence, or
classify a run. Durable evidence, run identity and pass/fail judgement belong
to the host (AYTHER Runtime replay QA).

## Contract and version

The contract lives in installed headers under `ayther/engine/`, namespace
`ayther::engine::audio_observation`:

| Header | Content |
|---|---|
| `audio_observer.hpp` | `Observer`, `FactView`, `PcmView`, field and identity types, limits |
| `audio_fact_queue.hpp`, `audio_pcm_queue.hpp` | bounded SPSC hand-off queues |
| `audio_observation_overflow.hpp`, `audio_observation_close.hpp` | out-of-band loss counter and close snapshot |
| `audio_observation_worker.hpp` | consumer worker thread |
| `audio_initial_snapshot.hpp` | owned snapshot of HD audio state |
| `audio_hd_state.hpp` | HD audio state header, payloads and validation |
| `audio_production_limit.hpp` | freeze, finalization and drain results |

`contract_version` is 1.0 and `supports()` accepts exactly 1.0. The version is
independent of the Engine release, the core ABI, the replay format, the render
observation and the HD state version.

Fact kinds and field names below are the producer schemas of contract 1.0.
The header does not advertise capabilities: replay, digital capture and full
trace completeness are not announced as Engine capabilities.

### Provider support

`Config::audio_observer` with a non-null `on_fact` or `on_pcm` is accepted only
with `EmulationProvider::libretro`. A `tessera_native` session that sets it
fails to create with `NativeStatus::unsupported_capability`. The HD-state,
freeze and drain operations do not check the provider, but all their tests
run on libretro.

## Installing an observer

1. Set `AytherSession::Config::audio_observer` (`context`, `on_fact`,
   `on_pcm`) before `AytherSession::create`. A null callback disables that
   stream; with both null the session follows its ordinary path and consumes no
   identities.
2. To install or remove only the PCM receiver after preparation, call
   `set_audio_pcm_observer(callback)`. It requires audio enabled and resets the
   observed main-output timeline so the first delivered block belongs to the
   prepared session state.
3. The host owns `context` and keeps it alive until the session is destroyed
   and its producers are stopped. Binding changes are permitted only while all
   affected producers are stopped.

Callback rules (normative):

- Every view, span and string borrows producer storage for the duration of the
  synchronous call only. Copy everything you keep before returning.
- A callback must not reenter or mutate the Engine, perform I/O, serialize,
  wait, throw, or allocate unbounded storage.
- Separate producers may call concurrently; the receiver supplies a bounded,
  non-blocking hand-off.
- A copy failure is recorded out of band as observation loss and never changes
  an audio decision. No observation result feeds back into scheduling or
  playback.

## Data model

| Type | Meaning |
|---|---|
| `FactId` | `{producer, sequence}`, both non-zero; run-local. The host adds its run identity when copying. Arrival order across producers is not causality. |
| `OccurrenceId` | Identity of one playback occurrence, distinct from business key, track or assignment even when those repeat. |
| `FactView` | `id`, `kind`, `frame` (`FramePosition`), `causes`, `state_orders`, `fields`. |
| `FieldView` | `name`, `availability`, `unit`, `value`, `unavailable_reason`. |
| `Cause` | A `FactId`, or `PreexistingContext{state_id}` naming an explicitly observed initial-state record. |
| `PcmView` | `id`, `capture_point`, `range` (`SampleRange`), `format`, `channels`, `bytes`, `causes`. |
| `SampleRange` | `timeline`, `sample_rate`, `[begin, end)` in sample frames. |

Field rules:

- `Availability` is `known`, `unknown` or `not_applicable`. A known field has a
  non-monostate value and no reason; unknown and not-applicable fields carry
  monostate and a non-empty technical reason. A missing value is never written
  as zero.
- `Unit` is one of `none`, `emulation_frame`, `sample_frame`, `bytes`, `count`,
  `linear_gain`, `frames_per_second`, `nanoseconds`. A sample frame holds one
  sample per channel.
- Empty `causes` do not imply complete observation. Values that cannot be
  represented stay unknown with a reason (for example `interval_overflow`);
  `UINT64_MAX` never wraps to zero.

Limits (`audio_observer.hpp`):

| Constant | Value |
|---|---:|
| `max_causes`, `max_state_orders` | 256 |
| `max_fields` | 128 |
| `max_text_bytes` | 4096 |
| `max_encoded_fact_bytes` | 64 KiB |
| `max_pcm_bytes` | 256 KiB |
| `max_sample_rate` | 192000 |
| `max_channels` | 8 |

Producer identities (`FactId::producer`): `session` 1, `pack` 2,
`detector_input` 3, `detector` 4, `selection` 5, `mixer` 6, `main_output` 7,
`postmix` 8, `auxiliary_output` 9. Each producer's sequence orders its own
facts only; when it is exhausted the producer marks its emission incomplete
and audio continues unchanged.

### Timelines

| Timeline | Clock |
|---|---|
| emulation frame (`FramePosition`) | 64-bit session frame |
| `detector_frame` field | the 32-bit frame the detector receives, kept apart from the session frame |
| `engine_main_mix` | mixer sample frames at 44100 Hz |
| `hd_asset_pcm` | source position inside a converted HD asset, 44100 Hz |
| `synth_input` | auxiliary (synth) stream input, at its own rate |
| `engine_main_output` | device line after rate control, at the device rate; the line of `PcmView` |

Positions on different timelines are related only by an observed mapping fact,
never by arrival order, wall-clock time or rounding.

## Producers

### Pack catalogue

Emitted while `audio_events.toml` is parsed by the Rust core parser (the same
rules as `events_from_toml`; observation adds no parser or acceptance rules).
The additive C entry point `ayther_audio_events_parse_observed` keeps the
existing ABI unchanged; its 1.0 view is 72 bytes on supported 64-bit targets.
Texts carry a length, not a terminator. It returns the total accepted even when
`cap` is smaller, and callbacks are not trimmed to `cap`.

| Fact | Content |
|---|---|
| `pack_catalog_begin` | origin (`pack` or `provided_text`) and declared count before filtering; its `FactId` identifies this catalogue read |
| `pack_assignment_declared` | ordinal, textual and canonical signature, declared asset and sizes; cause: catalogue begin |
| `pack_assignment_parse_result` | accept or reject from the parser branch, stage `catalog_parser`, reason; cause: the declaration |
| `pack_catalog_end` | observed declarations, parser acceptances, duplicates, `inventory_complete` |
| `pack_catalog_unavailable` | no interpretable inventory; origin and reason. Not an empty inventory. |

- An entry is identified by catalogue plus ordinal, even when signatures
  repeat. `duplicate_of_ordinal` names the first declaration with the same
  canonical signature (aliases such as `0x1` and `0X01` included); a first
  declaration has it `not_applicable`.
- The accepted count is not a count of unique keys, decoded assets, selections
  or plays.
- Missing pack or file, empty file, read failure, invalid TOML and a missing or
  non-array `event` member are distinguished from `event = []`.
- The adapter keeps up to 4096 signatures and 4096 accepted-entry links in
  fixed storage (at most 512 KiB). Beyond that, declarations are still emitted,
  duplicate status may be unknown and the inventory is marked incomplete; more
  than 4096 acceptances still load. Texts over 4096 bytes keep their size with
  reason `text_limit`. These limits never trim the catalogue the Engine
  processes.

### Effective load

Emitted after the session updates its assignment map and after its usual asset
preparation, without reinterpreting the catalogue.

| Fact | Content |
|---|---|
| `pack_assignment_loaded` | stage `session_assignment_map`, signature, effective asset, channels, `parsed_duration`/`parsed_span`/`parsed_looping`, reason `inserted` or `replaced_existing_signature`; causes: the parse result and, for a replacement, the earlier load of that signature |
| `pack_assignment_asset_result` | stage `asset_prewarm`, whether preparation was attempted now, source `pack`/disk when chosen in this attempt, cache presence and state, PCM bytes, reason (`already_prewarmed`, `empty_asset`, `not_cached`, …) |
| `pack_assignment_load_end` | updates performed, distinct keys loaded, `load_observation_complete` |

A rejected entry produces no load fact. A missing or corrupt asset may belong
to a loaded assignment and is reported as such, not as a play or a map
rejection. Cache inspection only reads state: it does not decode, open files or
retry. `load_observation_complete` certifies emission, not a complete load.

### Detector input and sources

Before each `process_frame_ex` call, for the `live` and `analysis` detectors:

| Fact | Content |
|---|---|
| `detector_input_batch` | batch identity, detector, `detector_frame`, write origin, write and PCM-event counts, `cross_stream_order = independent` |
| `detector_input` | type `chip_write` (chip, address, data, cycle) or `pcm_event` (type, channel, envelope, pan, start, loop, step), origin, `source_index`; cause: its batch |

- `cycle` is a CPU cycle within the frame, not an identity or a sample.
- Origins: `abi_snapshot` or `legacy_core` for writes, from the branch the
  emulator observer actually used; `core_event_queue` for typed PCM events.
- `source_index` keeps array order. No causality is implied between FM/PSG and
  PCM, or between an input and a detected event.
- An empty batch proves the empty arguments of that call, not that the source
  was active. Core events filtered before the detector are not reported as
  processed inputs.

Each observed `produce_frame` emits two `audio_source_interval` facts before
feeding the detector, one for `chip_writes` and one for `typed_audio_events`:

- the interval is `[begin_frame, begin_frame + 1)`; repeated or backward
  frames are not merged;
- state is `active`, `disabled`, `unavailable`, `interrupted` or `unknown`, with
  a reason. `active` requires an effective subscription and an interpretable
  read; a missing capability is never a verified zero;
- for writes: origin, reported count, received data, prepared inputs, ABI read
  result, snapshot validity, generation, overflow and active mask. A current
  snapshot with zero writes, active subscription and no overflow certifies
  zero. A fallback after an ABI read error stays `interrupted` even with a zero
  count. Overflow proves loss but its amount is unknown (the provider exposes
  one bit);
- for typed events: poll outcome, query states, reported versions and sizes,
  received count, prepared PCM and the drop counters of the real branches
  (`filtered_non_pcm`, `filtered_schema`, `filtered_type`). A failed read leaves
  `received` unknown. A mismatch between prepared plus filtered and polled is
  `typed_count_mismatch`;
- transport loss keeps the total before the poll and, when provable, the
  difference since the previous poll (continuous frames, interpretable and
  unsaturated counter). Without a baseline a zero total proves zero loss and a
  larger total is unknown. Counter regression, saturation (`UINT32_MAX` is kept
  as is), frame jumps or invalid statistics prevent attributing a difference.

After processing, `audio_detector_interval` reports detector, availability,
`call_completed`, whether substitution was enabled, and prepared, submitted and
not-submitted inputs, linking both source facts and the batch. Submitted means
arguments of a completed call, not recognized events. Disabled substitution is
not an inactive detector.

The extra subscription query is read-only and runs only while observing. Poll
policy, subscriptions and audio decisions do not change.

### Detector outputs

| Fact | Content |
|---|---|
| `detector_state_operation` | detector, session frame, `detector_frame`, action (`created`, `reset`, `process_frame`, `set_pal`, `set_initial_active`, `finish`, `clear_closed`), parameter, state order; processing links its input batch and the previous operation |
| `detector_output_batch` | use (`runtime_selection`, `live_learning`, `analysis_result`, `inspection`), capacity, return value, observed and total counts, `read_complete` |
| `detector_active_channel` | one row per active element: source index, signature, instrument, chip, channel, pitch |
| `detector_closed_event` | the same plus `start_frame`, `end_frame` and velocity exactly as the detector returned them (detector clock) |

- Creation or an observed reset establishes a known state. A missing
  antecedent or an unobserved mutation makes the history incomplete; resuming
  never invents the intermediate mutations.
- An active row is a snapshot, not a key-on. A closed row read after `finish`
  is not labelled as natural end or as caused by `finish`.
- The active-output result retains up to 64 links for later facts; a larger
  array is still emitted but marked incomplete. Closed rows are emitted without
  retention. The tracker is at most 64 bytes.
- Outputs are observed on the arrays the run already uses; no query is added
  for observation.

### Assignment matching

Every `resolve_event_sig` call carries one of eight uses: `live_assignment`,
`live_sequence_interest`, `live_voice_route`, `replay_trigger`,
`replay_voice_route`, `asset_coverage`, `bare_frame_mute`, `export_audio`.
Facts use producer `selection`.

| Fact | Content |
|---|---|
| `assignment_query` | signature, instrument, pitch, use, `source_kind`, `source_index`, `event_start`, `event_end`, `evaluated_frame`; cause: the observed source row |
| `assignment_exact_probe` | signature and `found` of the exact lookup; cause: the query |
| `assignment_candidate` | entry signature, rule, pitch (`not_applicable` for the exact candidate), origin, `visit_index` from zero |
| `assignment_candidate_decision` | `pitch_rejected`, `winner_updated` or `lower_priority`, with best provisional signature and rank |
| `assignment_selection` | once per query: `selected` (`exact_lookup` for an exact hit) or `invalid_instrument`, `empty_index`, `unknown_instrument`, `no_applicable_candidate` |

- A missing or zero source is `unknown` with `provenance_complete = false`.
- Live queries link the detector row actually consumed; queries over analysed
  events link the closed batch and row index. No inferred chip write and no
  row of another batch with the same signature is linked.
- Candidates follow the real `equal_range` walk of `AudioMatchIndex` before the
  pitch filter, keeping duplicates and incompatible notes. Order is the hash
  index traversal and is not guaranteed across builds. A visit is not a
  selection.

### Sequence anchoring

`seq_anchor_frame_observed` and `seq_anchor_table_observed` share the ordinary
algorithm; the ordinary functions delegate with empty callbacks. Contexts are
`live_pack`, `live_authored` and `replay_table`; the replay table is observed
only when its cache is regenerated.

| Fact | Content |
|---|---|
| `sequence_query` | input, substitution and state counts, source availability, `state_will_reset`, `origin_storage`, `anchor_frame` |
| `sequence_input` | signature, input index, source (`detector_signature`, `resolved_assignment`, `sequence_rule_trigger`, or the replay order), original index |
| `sequence_signature_transform` | original and target signature when a resolution or sequence rule substitutes another signature |
| `sequence_substitution`, `sequence_signature` | key, trigger, duration, span, enabled, looping, member and head sizes; each member signature with its index and list |
| `sequence_state_input` | `next_free`, `win_start`, `win_end`, `open`, and `will_be_used` |
| `sequence_candidate_visit` | stage `disabled`, `internal`, `not_head`, `formed` or `accumulated`; `trigger` and `head_hits` only where computed |
| `sequence_candidate_decision` | `quorum_rejected`, `claimed_by_open_sequence`, `selected` or `contradicted_open_sequence` |
| `sequence_selection` | per frame: `no_candidate`, `no_selection` or `selected`, with the real anchor count |

- A transformed signature needs its transform fact for complete provenance;
  otherwise it is `unknown`. No equal signature is searched to guess an origin.
- `contradicted_open_sequence` is rule 6: an open window closes when none of
  its voices still sounds and the opening of another exclusive sequence
  arrives. When the arrival is the opening of the same open sequence, rule 6
  re-anchors it like rule 5: the frame reports `selected` before the planned
  step and the HD playback restarts from zero.
- The session reserves origin storage for 4160 entries (4096 sequence triggers
  and 64 live voices, at most 512 KiB) once, only while observing. On failure
  or overflow audio continues and `origin_storage` is `allocation_failed` or
  `limit_exceeded`.

### HD playback

Each direct HD playback keeps an `OccurrenceId` from its first rising edge.

| Fact | Content |
|---|---|
| `hd_playback_request` | the occurrence; cause: `assignment_selection` |
| `hd_playback_decision` | `start` (`rising_edge`), `maintain` (`active_without_rising_edge`), `restart` (`same_key_same_asset`) or `replace` (`same_key_new_asset`); `previous_occurrence` and `previous_request` for shared-key decisions |
| `hd_playback_effect` | `started`, `start_failed`, `deferred` or `retained`; `played` is not applicable for `maintain` |

The mixer decides restart versus replace by comparing the shared PCM of the
newest voice with the same key. A shared-key decision has two causes: the new
request and the request that created the existing voice. Maintains never
replace that antecedent.

### Voice lifetime

| Fact | Content |
|---|---|
| `hd_voice_position_span` | occurrence, key, causal request, `[output)` on `engine_main_mix`, `[source)` on `hd_asset_pcm`, source limit, 44100 Hz; frame unknown with reason `mixer_sample_timeline_only` |
| `hd_voice_loop_crossing` | output position, source before and after, loop begin and end, asset limit |
| `hd_voice_lifetime_effect` | phase `begin`, `advance` or `end` of a `window_tail` or `authored_fade`; fade carries remaining samples before and after (not applicable for a tail) |
| `hd_voice_end` | reason `natural_end`, `window_end`, `window_cut`, `tail_complete`, `tail_cut`, `fade_complete` or `test_end`; source position and limit |

- A span groups contiguous one-to-one samples; a mix block boundary or a loop
  crossing closes it. Voices without a QA identity are mixed but emit nothing.
- The end reason comes from the branch that retired the voice, never from
  cursor proximity. Mixer-decided ends know the output position and leave the
  frame unknown; window-decided ends know the frame and leave the output
  position unknown.

### Mix

`hd_mix_participant` is emitted for each linear span a voice contributes to the
real mix block: occurrence and key, the request that created the voice, the
range on `engine_main_mix`, start and end on `hd_asset_pcm`, track limit,
`effective_gain_begin` and `effective_gain_end` (`linear_gain`),
`muted_by_gain` (the whole span at zero gain) and `nonzero_contribution` (some
sample contributed non-zero PCM before saturation). A zero-gain voice still
advances and stays active. The values come from the same loop that computes
the mix; PCM is not traversed twice.

`original_audio_span` covers native batches entering the common block before
mute and before HD voices: exact range on `engine_main_mix`, source hash, state
and reason.

| State | Meaning |
|---|---|
| `present` (`unmuted`) | the original path remains in the block; not a claim of audibility |
| `suppressed` | muted by hash, or by explicit interval suppression |
| `absent` (`no_native_batch`) | the voice router replaced the native PCM; hash not applicable |
| `unknown` (`capacity_exceeded`) | more than 256 native batches in one block; earlier spans are marked incomplete |

`audio_frame_sample_boundary` records where each executed frame starts on
`engine_main_mix` (frame, absolute position, offset within the staged block,
44100 Hz). Each `original_audio_span` names the boundary of the frame that
delivered its samples as a cause.

### Device output

With a PCM receiver installed, every buffer the session's logical-device
postmix callback receives is published as a `PcmView`: capture point
`sdl_logical_device_postmix`, timeline `engine_main_output`, consecutive range,
device rate, `f32_le`, two channels. The bytes are the real buffer, not a
remix. A format, length or size outside the contract makes the observation
incomplete.

`audio_frame_output_boundary` places the start of each audible frame on
`engine_main_output`:

| Field | Unit | Meaning |
|---|---|---|
| frame (`FramePosition`) | emulation frame | always known |
| `output_timeline` | — | `engine_main_output` |
| `output_position` | sample frame | first output sample whose resample position reaches the frame's first sample at the emulator stream input |
| `sample_rate` | Hz | device rate |
| `resample_rate_q32` | 32.32 fixed point | input samples per output sample under the rate-control ratio in force |
| `valid` | — | false if earlier boundaries were lost to capacity |

- Emitted once per frame that reaches the device, from the device thread, only
  when both fact and PCM receivers are installed. Silent production
  (`AudioOutputMode::silent`) and audio cut before delivery emit nothing.
- The device thread follows the SDL 3.4.8 resample position with the
  rate-control ratio read under the stream lock; a stream flush (cut, restore
  or new session) restarts tracking. `audio_qa_frame_output_boundary` requires
  each frame step to cross half its level within ±2 samples of its boundary.

Auxiliary (synth) stream:

| Fact | Content |
|---|---|
| `auxiliary_output_span` | the delivery's range on `synth_input`, its range on `engine_main_output`, both rates, Q32 step and initial phase, left and right filter support, `inserted_silence`; causes: the delivery and the containing PCM block |
| `auxiliary_delivery_loss` | range on `synth_input` and reason `delivery_failed` (SDL rejected it) or `discarded` (a cut removed pending audio); no PCM, never turned into a span or silence |

- `prime_synth` silence is a delivery with `inserted_silence = true`.
- Input ranges are the samples whose filter support participates in the output
  range, so lengths may differ and neighbouring deliveries may share
  transition samples. Phase advances only by synth frames SDL actually
  consumed; `SDL_ClearAudioStream` resets it.
- The support model is pinned to SDL 3.4.8; another version marks the mapping
  incomplete. Metadata travels through a fixed SPSC ring of 256 deliveries and
  up to 32 spans per block; overflow marks the mapping incomplete without
  touching SDL calls or audio.

### Neutrality

Adapters receive results already computed or follow the same real traversal.
They do not sort candidates, break ties, requery rules, or change mute,
selection, start, loop or end policy. The session wires them only while an
observer exists. Integration tests run the same core, ROM and inputs with
observation off, fully received, and with a receiver that drops after 64
callbacks; the three produce identical serialized state, anchors and exported
WAV, with exact counts of queries, candidates and selections.

## Transport helpers

The Engine provides bounded hand-off primitives for hosts; it does not use
them to write evidence.

- `BoundedFactQueue<Capacity>` and `BoundedPcmQueue<Capacity>`: SPSC queues
  with compile-time capacity. The producer copies the fact or block and all
  nested views into a preallocated slot without allocating, waiting,
  serializing or calling the consumer. PCM slots hold up to 256 KiB.
  `try_push` returns `accepted`, `full`, `invalid` (exceeds contract limits;
  no slot consumed) or `closed`. A full queue never overwrites or reorders.
- `ObservationOverflowCounter`: lives outside the slots; on `full` it counts
  the rejection and keeps the first affected `FactId` (facts) or timeline,
  rate and range (PCM). It is published with release ordering, read with
  acquire, and saturates at `UINT64_MAX`.
- `ObservationConsumerWorker`: runs a host-owned drain callback on its own
  thread. Producers never notify, lock or wait for it. Start, stop and join
  are control operations and must never run on the audio path.
- `close()` makes further `try_push` return `closed` while pending slots can
  still drain. `close_snapshot()` is complete only when the producer is closed,
  the last emitted and last consumed identities match, and no overflow
  occurred.

## HD audio state

These operations let a replay host start from a known HD-audio boundary.

### Snapshot and fresh start

- `audio_initial_snapshot()` returns an owned copy of the observable HD state:
  mixer voices (identity, key, position, limit, window, loop, gain, fade),
  sequence and live windows, and pending PCM (`main_staging`, `main_batch`,
  `main_stream`, `synth_stream`, `auxiliary_submission`). It does not step the
  core, mix or consume audio. A failed SDL queue query sets `complete = false`
  instead of reporting an empty queue.
- `prepare_fresh_hd_audio()` establishes the boundary used when a take has no
  complete, compatible HD state. It neither resets nor steps the game and keeps
  the catalogue, assignments and observers. It resets the live detector and
  clears windows, edges, instances, fired markers and learning; the player
  stops voices and previews, empties staging and streams, and resets sample
  lines, auxiliary queues, mutes, gains and telemetry. Decoded assets stay
  cached. It returns `false` when SDL could not prove inherited output was
  removed. The next snapshot reports `initialization = fresh`.

### State header and validation

`AudioHdStateHeader` carries the version (`kAudioHdStateVersion`, 1.1), an
opaque game-state identity that must name the exact restored emulator state,
the emulation frame, and a section mask. Sections (`AudioHdStateSection`):
`detector`, `windows`, `voices`, `requests`, `pending_audio`; all are required
(`kAudioHdStateRequiredSections`). Version 1.1 adds the category and bus of
each voice; a 1.0 state is rejected.

`validate_audio_hd_state(header, expected_identity, expected_frame)` returns
`compatible`, `unsupported_version`, `missing_expected_identity`,
`missing_identity`, `identity_too_long` (over 256 bytes), `identity_mismatch`,
`frame_mismatch`, `missing_sections` or `unknown_sections`, with the missing
and unknown masks.

### Restore

Each restore validates the header and the complete payload, builds the new
state aside, and swaps only when everything is valid; a rejection leaves the
session unchanged. Results are `AudioHdRestoreResult` with code `restored`,
`incompatible_header`, `incomplete_payload`, `invalid_detector`,
`invalid_window`, `invalid_pcm`, `invalid_voice`, `invalid_request`,
`invalid_pending_audio`, `duplicate_value` or `resource_limit`. Restores
replace state; they never merge with the previous run, process inputs or
advance the frame.

| Payload | Read / restore | Content and limits |
|---|---|---|
| `AudioHdDetectorWindowsState` | `audio_hd_detector_windows_state()` / `restore_audio_hd_detector_windows()` | opaque detector state (own version, 64-bit signatures, at most 1 MiB), previous active signatures and mask, sequence windows, next anchors, signature–instrument learning; at most 4096 entries per collection |
| `AudioHdVoicesState` | `audio_hd_voices_state()` / `restore_audio_hd_voices()` | mix-ready PCM assets stored once and voices referencing them: occurrence and cause, key, cursor, output position, gain, kind, window limits, fade, tail, loop region, delay; at most 256 voices and 64 MiB of PCM. Voices are identified by occurrence, so two voices may share key and asset; a duplicate occurrence is rejected. An overload returns the voices without copying PCM, sharing the mixer's buffers. |
| `AudioHdRequestsPendingState` | `audio_hd_requests_pending_state()` / `restore_audio_hd_requests_pending()` | logical HD instances, sequence and event fired markers, main audio still in staging (S16 stereo PCM, batches with hash and range, mutes, original-audio provenance, output cursors, auxiliary counters); at most 4096 entries, 256 provenance batches and 16 MiB of pending PCM |

A requests/pending restore removes inherited backlog from the SDL streams
before input zero, and restored occurrences reserve their identity range. SDL
cannot read its queues without consuming them, so a capture with bytes already
delivered to a stream, an auxiliary conversion with non-zero phase, an
auxiliary delivery in progress, or an inherited HD stream is marked incomplete
and is not accepted as compatible.

Restore order with a checkpoint: emulator state, visual state
([Visual state](VISUAL_STATE.md)), then detector/windows, voices and
requests/pending.

### End of a replay

- `freeze_audio_production()` fixes an immutable `AudioProductionLimit` after
  the last requested frame: `last_emulation_frame`, `main_sample_limit` (first
  sample frame that may not be produced; sent timeline plus staged frames,
  independent of device drain), `pending_main_frames`,
  `delivered_output_samples_at_freeze`, `auxiliary_input_limit`. Afterwards
  `step()` does not advance the core, and the player rejects new native or
  auxiliary input, priming silence, voices, previews and pending-state
  restores. Repeated calls return the same limit.
- `finalize_audio_hd_voices_for_test()` is accepted only after freezing. It
  retires every active voice with one `hd_voice_end` (`test_end`) per
  occurrence, using the frozen main limit as output position (or only the final
  frame when the limit is incomplete). It mixes nothing, completes no loop,
  fade or tail, never fabricates `natural_end`, and is idempotent.
- `drain_frozen_audio()` requires a frozen limit and no active voice. It
  delivers only staging already inside `main_sample_limit`, without stall
  priming, mixing or stepping. `AudioFrozenDrainResult` reports `accepted`,
  `complete`, `drained_main_frames`, `remaining_main_frames`,
  `main_sample_limit`, `output_complete` and `output_sample_limit`. On an SDL
  rejection staging stays pending and `complete` is false. Repeated calls never
  duplicate samples; they report when every bound stream reached the logical
  device and pause it at the exact postmix boundary.

### Pause and resume

`pause_after_drain(limit)` waits up to `limit` until the device has played
everything produced, then stops the HD transport, which keeps its state
(`AudioDrainResult`: `drained`, `timed_out` with `remaining_frames`, or
`unavailable`). Do not step while paused. `resume_transport()` does not start
the device immediately: the first PCM delivery, that of frame k+1, starts it,
without silence priming even when the pause exceeded the 250 ms stall
detector. Frame k+1 therefore follows frame k on `engine_main_output` without
inserted silence, and rate control rebuilds the buffer. The resume offset of a
frame is the presentation time of its image minus its
`audio_frame_output_boundary` position converted with `sample_rate`
(`audio_qa_frame_output_resume`).

## Verification

- `tests/audio_qa/observer_contract/`: one unit or integration test per
  producer, queue, restore and end-of-replay operation, using synthetic
  catalogues, a synthetic signed pack, the repository test core and synthetic
  ROMs.
- `tests/audio_qa/`: probes and oracles over the audio player, mixer and SDL
  conversion; see [Audio QA probes](../tests/audio_qa/README.md).

These are Engine component and integration tests. They are not a replay
campaign on real games and do not establish product acceptance.
