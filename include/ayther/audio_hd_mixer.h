#pragma once
// ---------------------------------------------------------------------------
// audio_hd_mixer.h — HD voice mixer on the main stream's SAMPLE timeline.
//
// THE PROBLEM IT SOLVES. HD replacements used to run in their own SDL streams:
// they started "now" according to wall-clock time while the original traveled
// through `emu_stream_` with a ~70 ms DRC cushion. The phase between original
// and HD therefore depended on backlog, stalls, and catch-up size. Here every
// voice is PLACED at an absolute sample on the staged block timeline and mixed
// INSIDE that block: a trigger at frame N lands on the same sample under 1x1
// execution or catch-up 16, and everything—original, router, and HD—crosses
// the SAME DRC/backlog. Pausing one stream pauses everything.
//
// WHAT THIS MODULE IS. Mixing only: voices with already decoded and converted
// PCM (S16 stereo at 44100 Hz, guaranteed by the AudioPlayer cache), sample
// placement, phase-preserving loops, gain, cut fades, and the per-frame
// lifetime contract (end + tail). It does NOT touch SDL: mixing is a pure
// function over a buffer, so the 1x1-vs-catch-up identity oracle can be exact,
// byte for byte, without a device.
//
// The FRAME-based lifecycle (`end_frame`/`cut_frame`) deliberately remains in
// frames: it is the same contract used by `tick_events` and session windows.
// Frame-to-sample conversion lives in ONE place (start placement), rather than
// being scattered across every sweep.
// ---------------------------------------------------------------------------

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <ayther/audio_playback_policy.h>
#include <ayther/engine/audio_hd_state.hpp>
#include <ayther/engine/audio_initial_snapshot.hpp>

/// Shared mix-ready PCM: interleaved stereo S16 at 44100 Hz. Shared
/// (shared_ptr) because N voices of the same asset must not duplicate the
/// decode, and because a voice must survive a cache invalidation without
/// dangling.
using HdMixPcm = std::shared_ptr<const std::vector<int16_t>>;

class HdMixer {
public:
  /// Cut fade, in FRAMES at 44100 (~60 ms — the same audible criterion as
  /// the stop_sfx_by_key fade in the stream path).
  static constexpr uint32_t kFadeFrames = 2646;

  enum class StartAction : uint8_t {
    start,
    continue_playback,
    restart,
    overlap,
    replace
  };

  struct StartResult {
    StartAction action = StartAction::start;
    uint64_t previous_occurrence = 0;
    uint32_t previous_cause_producer = 0;
    uint64_t previous_cause_sequence = 0;
    uint64_t new_occurrence = 0;
    bool voice_created = false;
  };

  struct VoiceIdentity {
    uint64_t occurrence = 0;
    uint32_t cause_producer = 0;
    uint64_t cause_sequence = 0;
  };

  struct PositionSpan {
    VoiceIdentity identity;
    uint64_t key = 0;
    uint64_t output_begin = 0;
    uint64_t output_end = 0;
    uint64_t source_begin = 0;
    uint64_t source_end = 0;
    uint64_t source_limit = 0;
    float effective_gain_begin = 0.0f;
    float effective_gain_end = 0.0f;
    bool muted_by_gain = true;
    bool nonzero_contribution = false;
  };

  struct LoopCrossing {
    VoiceIdentity identity;
    uint64_t key = 0;
    uint64_t output_position = 0;
    uint64_t source_before = 0;
    uint64_t source_after = 0;
    uint64_t loop_begin = 0;
    uint64_t loop_end = 0;
    uint64_t source_limit = 0;
  };

  enum class EndReason : uint8_t {
    natural_end,
    window_end,
    window_cut,
    fade_complete,
    tail_complete,
    tail_cut,
    test_end
  };

  struct VoiceEnd {
    VoiceIdentity identity;
    uint64_t key = 0;
    EndReason reason = EndReason::natural_end;
    uint64_t source_position = 0;
    uint64_t source_limit = 0;
    uint64_t output_position = 0;
    uint64_t frame_position = 0;
    bool output_position_known = false;
    bool frame_position_known = false;
  };

  enum class EffectKind : uint8_t { tail, fade };
  enum class EffectStage : uint8_t { begin, advance, end };

  struct LifetimeEffect {
    VoiceIdentity identity;
    uint64_t key = 0;
    EffectKind kind = EffectKind::tail;
    EffectStage stage = EffectStage::begin;
    uint64_t source_begin = 0;
    uint64_t source_end = 0;
    uint64_t output_begin = 0;
    uint64_t output_end = 0;
    uint64_t frame_position = 0;
    uint32_t remaining_begin = 0;
    uint32_t remaining_end = 0;
    bool output_range_known = false;
    bool frame_position_known = false;
  };

  using PositionCallback = void (*)(void *, const PositionSpan &) noexcept;
  using LoopCallback = void (*)(void *, const LoopCrossing &) noexcept;
  using EndCallback = void (*)(void *, const VoiceEnd &) noexcept;
  using EffectCallback = void (*)(void *, const LifetimeEffect &) noexcept;

  void set_position_observer(void *context,
                             PositionCallback callback) noexcept {
    position_context_ = context;
    position_callback_ = callback;
  }

  void set_loop_observer(void *context, LoopCallback callback) noexcept {
    loop_context_ = context;
    loop_callback_ = callback;
  }

  void set_end_observer(void *context, EndCallback callback) noexcept {
    end_context_ = context;
    end_callback_ = callback;
  }

  void set_effect_observer(void *context, EffectCallback callback) noexcept {
    effect_context_ = context;
    effect_callback_ = callback;
  }

  struct Voice {
    uint64_t key = 0;
    uint64_t occurrence = 0; ///< QA identity; never a business key
    uint32_t cause_producer = 0;
    uint64_t cause_sequence = 0;
    HdMixPcm pcm;                    ///< stereo S16 44100 (mix-ready)
    uint64_t state_pcm_identity = 0; ///< identity used only by saved state
    uint64_t start_sample = 0;       ///< ABSOLUTE start sample
    size_t pos = 0;                  ///< cursor in FRAMES within the pcm
    uint64_t output_end = 0;         ///< first output sample after last mix
    bool output_end_known = false;
    float gain = 1.0f;
    bool looping = false;
    bool event = false;              ///< the play_event_hd contract
    uint64_t end_frame = UINT64_MAX; ///< end of window (FRAME domain)
    uint64_t cut_frame = UINT64_MAX; ///< end + tail; MAX = drains
    /// > 0 = fade frames remaining (the voice dies on reaching 0).
    uint32_t fade_left = 0;
    /// The number of frames the current fade STARTED with — the denominator
    /// of the ramp. Without this, an authored fade of 30,000 frames would be
    /// divided by `kFadeFrames` and would sit pinned at gain > 1 until the
    /// end.
    uint32_t fade_span = kFadeFrames;
    /// FADE_OUT end policy, in frames at 44100. 0 = no fade (hard_cut or
    /// tail, as configured). The ramp starts on passing `end_frame` and
    /// reaches silence `fade_frames` later — which is what the author asked
    /// for: "end in silence N frames after the limit".
    uint32_t fade_frames = 0;
    bool tail_active = false;
    bool fade_effect_active = false;
    /// Loop REGION within the asset, in FRAMES. `loop_end == 0` = no
    /// region: the whole asset repeats, which was the only option before.
    ///
    /// It only applies when `looping`: a region on a one-shot would mean
    /// nothing, and trimming it by that region would cut the sound in half.
    size_t loop_begin = 0;
    size_t loop_end = 0;
    /// Telemetry: how many samples late the start arrived (0 = in phase).
    uint64_t late_samples = 0;
    ayther::AudioCategory category = ayther::AudioCategory::effect;
  };

  /// Starts (or retriggers) a voice. `start_sample` = ABSOLUTE position on
  /// the stream timeline where its first frame falls — the caller computes it
  /// as (timeline + offset of the current frame within the staged block),
  /// which is what makes the placement immune to catch-up.
  /// `offset_frames` starts from the middle of the asset (resume, late
  /// trigger): for a loop it enters through the modulo (phase preserved).
  /// Retriggering the same key fades the previous voice out (the usual
  /// retrigger contract). Returns false with no usable PCM.
  bool start(uint64_t key, HdMixPcm pcm, uint64_t start_sample,
             uint64_t offset_frames, float gain, bool looping, bool event,
             uint64_t end_frame, uint64_t cut_frame, uint32_t fade_frames = 0,
             // loop region in FRAMES (0,0 = the whole asset).
             size_t loop_begin = 0, size_t loop_end = 0,
             const VoiceIdentity *identity = nullptr,
             StartResult *result = nullptr,
             ayther::RepeatPolicy repeat_policy = ayther::RepeatPolicy::restart,
             ayther::TransitionPolicy transition_policy =
                 ayther::TransitionPolicy::cut,
             ayther::AudioCategory category = ayther::AudioCategory::effect) {
    const VoiceIdentity observed =
        identity ? *identity : VoiceIdentity{0, 0, 0};
    if (result)
      *result =
          StartResult{StartAction::start, 0, 0, 0, observed.occurrence, false};
    if (!pcm || pcm->size() < 2)
      return false;
    // Read the effective predecessor before stop() changes its fade state.
    // Reverse order selects the newest voice when an older retrigger is
    // still draining its fade.
    bool stop_predecessor = true;
    bool replace_predecessor = false;
    bool matched_key = false;
    uint64_t predecessor_key = key;
    for (auto it = voices_.rbegin(); it != voices_.rend(); ++it)
      if (it->key == key) {
        matched_key = true;
        if (result) {
          result->action =
              it->pcm == pcm
                  ? repeat_policy == ayther::RepeatPolicy::continue_playback
                        ? StartAction::continue_playback
                    : repeat_policy == ayther::RepeatPolicy::overlap
                        ? StartAction::overlap
                        : StartAction::restart
                  : StartAction::replace;
          result->previous_occurrence = it->occurrence;
          result->previous_cause_producer = it->cause_producer;
          result->previous_cause_sequence = it->cause_sequence;
        }
        if (it->pcm == pcm &&
            repeat_policy == ayther::RepeatPolicy::continue_playback) {
          if (result)
            result->new_occurrence = it->occurrence;
          return true;
        }
        stop_predecessor =
            it->pcm != pcm || repeat_policy != ayther::RepeatPolicy::overlap;
        replace_predecessor = it->pcm != pcm;
        break;
      }
    // Music and ambience are exclusive buses. Their logical identity is the
    // authored signature (`key`), so a transition normally arrives with a
    // different key. Key-only replacement would leave every previous track
    // alive and mix Wilderness, Battle and bonus together.
    const bool exclusive_category = category == ayther::AudioCategory::music ||
                                    category == ayther::AudioCategory::ambient;
    if (!matched_key && exclusive_category) {
      for (auto it = voices_.rbegin(); it != voices_.rend(); ++it)
        if (it->category == category) {
          replace_predecessor = true;
          predecessor_key = it->key;
          if (result) {
            result->action = StartAction::replace;
            result->previous_occurrence = it->occurrence;
            result->previous_cause_producer = it->cause_producer;
            result->previous_cause_sequence = it->cause_sequence;
          }
          break;
        }
    }
    if (replace_predecessor) {
      if (transition_policy == ayther::TransitionPolicy::fade)
        stop_with_fade(predecessor_key,
                       fade_frames ? fade_frames : kFadeFrames);
      else
        stop_hard(predecessor_key);
    } else if (stop_predecessor) {
      stop(key); // configured restart/replace: predecessor fades out
    }
    Voice v;
    v.key = key;
    v.occurrence = observed.occurrence;
    v.cause_producer = observed.cause_producer;
    v.cause_sequence = observed.cause_sequence;
    v.start_sample = start_sample;
    v.gain = gain;
    v.looping = looping;
    v.event = event;
    v.end_frame = end_frame;
    v.cut_frame = cut_frame;
    v.fade_frames = fade_frames;
    v.category = category;
    const size_t frames = pcm->size() / 2;
    // The region is SANITISED here and not while mixing: a `loop_end`
    // larger than the asset or inverted would read past the buffer, and
    // checking it per sample would mean paying that check 44,100 times a
    // second for something that never changes over the life of the voice.
    if (loop_end > frames)
      loop_end = frames;
    if (loop_begin >= loop_end) {
      loop_begin = 0;
      loop_end = 0;
    }
    v.loop_begin = loop_begin;
    v.loop_end = loop_end;
    v.pos = looping ? static_cast<size_t>(offset_frames % frames)
                    : static_cast<size_t>(offset_frames);
    // Resuming inside a loop that has a region: if the offset falls
    // outside the cycle, it enters at its phase WITHIN the region. Leaving
    // it before the start would make the resume replay the intro again.
    if (v.loop_end && v.pos >= v.loop_end) {
      const size_t span = v.loop_end - v.loop_begin;
      v.pos = v.loop_begin + ((v.pos - v.loop_begin) % span);
    }
    v.pcm = std::move(pcm);
    // Non-loop with the offset past the end: nothing to mix — success
    // without a voice, the same contract as the stream path.
    if (!v.looping && v.pos >= frames)
      return true;
    voices_.push_back(std::move(v));
    if (result)
      result->voice_created = true;
    ++started_;
    return true;
  }

  /// Fade cut (~60 ms) of every voice of a key. true if it reached one that
  /// was not already fading — the same observable contract as
  /// stop_sfx_by_key (telling "I cut it short" apart from "there was
  /// nothing").
  bool stop(uint64_t key) { return stop_with_fade(key, kFadeFrames); }

  bool stop_with_fade(uint64_t key, uint32_t frames) {
    if (frames == 0)
      return stop_hard(key);
    bool cut = false;
    for (Voice &v : voices_)
      if (v.key == key && v.fade_left == 0) {
        v.fade_left = frames;
        v.fade_span = frames;
        cut = true;
      }
    return cut;
  }

  /// Immediate HARD cut (pause / seek): no fade, no residue — the staging is
  /// discarded whole on those paths and the voice must not drain.
  void cut_all() { voices_.clear(); }

  /// Closes every active voice at the already-frozen QA boundary. This emits
  /// one end observation per voice without mixing, advancing a source cursor,
  /// completing a loop, or synthesizing a tail. Repeated calls are harmless.
  size_t finalize_for_test(uint64_t output_position, uint64_t frame_position,
                           bool output_position_known) noexcept {
    const size_t finalized = voices_.size();
    for (const Voice &voice : voices_)
      notify_end(voice, EndReason::test_end, output_position, frame_position,
                 output_position_known, !output_position_known);
    voices_.clear();
    return finalized;
  }

  /// Starts a new independent HD-audio session while preserving observer
  /// callbacks. No voice or telemetry from the previous session survives.
  void reset_session_state() noexcept {
    voices_.clear();
    started_ = 0;
    skew_samples_ = 0;
    max_skew_ = 0;
  }

  /// Hard cut filtered by class: the session paths distinguish one-shots
  /// (stop_all_sfx) from event streams (stop_all_events) and the unified mode
  /// has to respect that boundary — a seek that invalidates the take events
  /// must not silence an unrelated one-shot.
  void cut_all_of(bool event) {
    for (size_t i = voices_.size(); i-- > 0;)
      if (voices_[i].event == event)
        voices_.erase(voices_.begin() + static_cast<std::ptrdiff_t>(i));
  }

  bool stop_hard(uint64_t key) {
    const size_t n = voices_.size();
    for (size_t i = voices_.size(); i-- > 0;)
      if (voices_[i].key == key)
        voices_.erase(voices_.begin() + static_cast<std::ptrdiff_t>(i));
    return voices_.size() != n;
  }

  /// Live gain of a key (the slider of a Sequence while it plays).
  bool set_gain(uint64_t key, float gain) {
    bool any = false;
    for (Voice &v : voices_)
      if (v.key == key && v.fade_left == 0) {
        v.gain = gain;
        any = true;
      }
    return any;
  }

  /// The per-FRAME lifetime contract — an exact mirror of tick_events: past
  /// cut_frame the voice dies even with PCM left; a loop past its end_frame
  /// dies (no tail) or stops looping and drains until cut (tail).
  void tick_frame(uint64_t frame) {
    for (size_t i = voices_.size(); i-- > 0;) {
      Voice &v = voices_[i];
      // FADE_OUT. On passing the limit the voice does not die abruptly:
      // it starts a ramp of `fade_frames` frames and switches itself off
      // on reaching silence. It is an end policy ALTERNATIVE to tail
      // —they do not stack— and that is why it is evaluated BEFORE
      // cut_frame: the hard cut of the window must not interrupt the ramp
      // the author asked for. A fade already in progress is not
      // restarted.
      if (v.fade_frames && frame > v.end_frame) {
        if (v.fade_left == 0) {
          v.fade_left = v.fade_frames;
          v.fade_span = v.fade_frames;
          v.looping = false; // stops re-feeding itself: it fades out
          v.fade_effect_active = true;
          notify_effect(v, EffectKind::fade, EffectStage::begin, v.pos, v.pos,
                        v.output_end, v.output_end, frame, v.fade_left,
                        v.fade_left, v.output_end_known, true);
        }
        continue;
      }
      if (frame > v.cut_frame) {
        if (v.tail_active)
          notify_effect(v, EffectKind::tail, EffectStage::end, v.pos, v.pos, 0,
                        0, frame, 0, 0, false, true);
        notify_end(v,
                   v.tail_active ? EndReason::tail_cut : EndReason::window_cut,
                   v.output_end, frame, v.output_end_known, true);
        voices_.erase(voices_.begin() + static_cast<std::ptrdiff_t>(i));
        continue;
      }
      if (v.looping && frame > v.end_frame) {
        if (v.cut_frame == UINT64_MAX || v.cut_frame <= v.end_frame) {
          notify_end(v, EndReason::window_end, v.output_end, frame,
                     v.output_end_known, true);
          voices_.erase(voices_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
          v.looping = false; // tail: drains the rest until cut
          v.tail_active = true;
          notify_effect(v, EffectKind::tail, EffectStage::begin, v.pos, v.pos,
                        0, 0, frame, 0, 0, false, true);
        }
      }
    }
  }

  /// ADDS the voices onto `out` (interleaved stereo S16, `frames` frames),
  /// which represents samples [block_start, block_start + frames) of the
  /// timeline. Per-sample placement: a voice whose start falls WITHIN the
  /// block enters at its exact offset — that is what makes the result
  /// identical between 1×1 and catch-up. A voice scheduled for a block that
  /// already passed enters at the beginning and records its lateness (skew
  /// telemetry).
  void mix_into(int16_t *out, size_t frames, uint64_t block_start) {
    if (!out || frames == 0)
      return;
    for (size_t i = voices_.size(); i-- > 0;) {
      Voice &v = voices_[i];
      if (v.start_sample >= block_start + frames)
        continue;    // future
      size_t at = 0; // offset in FRAMES within the block
      if (v.start_sample > block_start) {
        at = static_cast<size_t>(v.start_sample - block_start);
      } else if (v.start_sample < block_start && v.pos == 0 &&
                 v.late_samples == 0 && !v.looping) {
        // First block of a voice that arrived LATE (e.g. scheduled
        // during a stall): record the lateness once.
        v.late_samples = block_start - v.start_sample;
        skew_samples_ += v.late_samples;
        if (v.late_samples > max_skew_)
          max_skew_ = v.late_samples;
      }
      const std::vector<int16_t> &pcm = *v.pcm;
      const size_t pcm_frames = pcm.size() / 2;
      bool done = false;
      // Where the cycle ends and where it returns to. With no region it
      // is the whole asset, which is the long-standing behaviour.
      const size_t cycle_end =
          (v.looping && v.loop_end) ? v.loop_end : pcm_frames;
      const size_t cycle_beg = (v.looping && v.loop_end) ? v.loop_begin : 0;
      EndReason completion_reason = EndReason::natural_end;
      bool completion_observed = false;
      const size_t effect_source_begin = v.pos;
      const uint32_t fade_remaining_begin = v.fade_left;
      bool span_open = false;
      size_t span_at = 0;
      size_t span_source = 0;
      size_t span_end = at;
      float span_gain_begin = 0.0f;
      float span_gain_end = 0.0f;
      bool span_has_gain = false;
      bool span_all_gain_zero = true;
      bool span_nonzero_contribution = false;
      const auto close_span = [&](size_t end_at) noexcept {
        if (!span_open || !position_callback_ || v.occurrence == 0)
          return;
        position_callback_(
            position_context_,
            PositionSpan{{v.occurrence, v.cause_producer, v.cause_sequence},
                         v.key,
                         block_start + span_at,
                         block_start + end_at,
                         span_source,
                         v.pos,
                         pcm_frames,
                         span_gain_begin,
                         span_gain_end,
                         span_all_gain_zero,
                         span_nonzero_contribution});
      };
      for (size_t f = at; f < frames && !done; ++f) {
        if (v.pos >= cycle_end) {
          if (!v.looping) {
            done = true;
            completion_reason = v.tail_active ? EndReason::tail_complete
                                              : EndReason::natural_end;
            completion_observed = true;
            break;
          }
          close_span(f);
          span_open = false;
          if (loop_callback_ && v.occurrence != 0)
            loop_callback_(
                loop_context_,
                LoopCrossing{{v.occurrence, v.cause_producer, v.cause_sequence},
                             v.key,
                             block_start + f,
                             v.pos,
                             cycle_beg,
                             cycle_beg,
                             cycle_end,
                             pcm_frames});
          // It returns to the START OF THE REGION, not of the file:
          // that is what lets a track have an intro and then cycle
          // without exporting two files.
          v.pos = cycle_beg;
        }
        if (!span_open) {
          span_open = true;
          span_at = f;
          span_source = v.pos;
          span_has_gain = false;
          span_all_gain_zero = true;
          span_nonzero_contribution = false;
        }
        float g = v.gain;
        if (v.fade_left > 0) {
          g *= static_cast<float>(v.fade_left) /
               static_cast<float>(v.fade_span ? v.fade_span : 1);
          if (--v.fade_left == 0) {
            done = true;
            completion_reason = EndReason::fade_complete;
            completion_observed = true;
          }
        }
        if (!span_has_gain) {
          span_gain_begin = g;
          span_has_gain = true;
        }
        span_gain_end = g;
        span_all_gain_zero = span_all_gain_zero && g == 0.0f;
        const size_t s = v.pos * 2;
        const size_t o = f * 2;
        for (int c = 0; c < 2; ++c) {
          const int32_t contribution =
              static_cast<int32_t>(static_cast<float>(pcm[s + c]) * g);
          span_nonzero_contribution =
              span_nonzero_contribution || contribution != 0;
          const int32_t mixed = static_cast<int32_t>(out[o + c]) + contribution;
          out[o + c] = static_cast<int16_t>(
              mixed > 32767 ? 32767 : (mixed < -32768 ? -32768 : mixed));
        }
        ++v.pos;
        span_end = f + 1;
        v.output_end = block_start + span_end;
        v.output_end_known = true;
      }
      close_span(span_end);
      if (span_end > at && (v.tail_active || v.fade_effect_active))
        notify_effect(
            v, v.fade_effect_active ? EffectKind::fade : EffectKind::tail,
            EffectStage::advance, effect_source_begin, v.pos, block_start + at,
            block_start + span_end, 0, fade_remaining_begin, v.fade_left, true,
            false);
      const bool natural_at_asset_end =
          !done && !v.looping && v.pos >= pcm_frames && v.fade_left == 0 &&
          v.start_sample <= block_start;
      if (natural_at_asset_end) {
        completion_reason =
            v.tail_active ? EndReason::tail_complete : EndReason::natural_end;
        completion_observed = true;
      }
      if (done || natural_at_asset_end) {
        if (completion_observed) {
          if (v.tail_active || v.fade_effect_active)
            notify_effect(
                v, v.fade_effect_active ? EffectKind::fade : EffectKind::tail,
                EffectStage::end, v.pos, v.pos, v.output_end, v.output_end, 0,
                v.fade_left, v.fade_left, v.output_end_known, false);
          notify_end(v, completion_reason, v.output_end, 0, v.output_end_known,
                     false);
        }
        voices_.erase(voices_.begin() + static_cast<std::ptrdiff_t>(i));
      }
    }
    mixed_samples_ += frames;
  }

  size_t voice_count() const { return voices_.size(); }
  void append_initial_snapshot(
      ayther::engine::audio_observation::AudioInitialSnapshot &snapshot) const {
    for (const Voice &voice : voices_) {
      snapshot.voices.push_back({
          voice.occurrence,
          voice.key,
          static_cast<uint64_t>(voice.pos),
          voice.pcm ? static_cast<uint64_t>(voice.pcm->size() / 2) : 0,
          voice.start_sample,
          voice.gain,
          voice.looping,
          voice.event,
          voice.end_frame,
          voice.cut_frame,
          static_cast<uint64_t>(voice.loop_begin),
          static_cast<uint64_t>(voice.loop_end),
          voice.fade_left,
      });
    }
  }
  size_t event_voice_count() const {
    size_t n = 0;
    for (const Voice &v : voices_)
      n += v.event ? 1 : 0;
    return n;
  }

  /// Copies all active voices and their shared PCM without advancing a cursor
  /// or invoking an observer. PCM identities are local to this state object.
  ayther::engine::audio_observation::AudioHdVoicesState voice_state() const {
    namespace obs = ayther::engine::audio_observation;
    obs::AudioHdVoicesState state;
    state.started = started_;
    state.mixed_samples = mixed_samples_;
    state.skew_samples = skew_samples_;
    state.max_skew_samples = max_skew_;
    state.voices.reserve(voices_.size());
    std::unordered_map<const std::vector<int16_t> *, uint64_t> identities;
    std::unordered_set<uint64_t> used_identities;
    identities.reserve(voices_.size());
    used_identities.reserve(voices_.size());
    for (const Voice &voice : voices_) {
      if (!voice.pcm || voice.pcm->empty() || voice.pcm->size() % 2 != 0) {
        state.complete = false;
        continue;
      }
      uint64_t identity = voice.state_pcm_identity;
      if (identity == 0 || used_identities.contains(identity)) {
        identity = 1;
        while (used_identities.contains(identity))
          ++identity;
      }
      const auto [entry, inserted] =
          identities.emplace(voice.pcm.get(), identity);
      if (inserted)
        used_identities.insert(entry->second);
      if (inserted)
        state.pcm_assets.push_back({entry->second, *voice.pcm});
      state.voices.push_back({
          voice.occurrence,
          voice.cause_producer,
          voice.cause_sequence,
          voice.key,
          entry->second,
          static_cast<uint64_t>(voice.pos),
          voice.start_sample,
          voice.output_end,
          voice.output_end_known,
          voice.gain,
          voice.looping,
          voice.event,
          voice.end_frame,
          voice.cut_frame,
          voice.fade_left,
          voice.fade_span,
          voice.fade_frames,
          voice.tail_active,
          voice.fade_effect_active,
          static_cast<uint64_t>(voice.loop_begin),
          static_cast<uint64_t>(voice.loop_end),
          voice.late_samples,
          voice.category,
      });
    }
    return state;
  }

  /// Atomically replaces active voices after validating the complete state.
  /// This is a control-path operation: it neither mixes nor emits samples.
  ayther::engine::audio_observation::AudioHdRestoreCode restore_voice_state(
      const ayther::engine::audio_observation::AudioHdVoicesState
          &state) noexcept {
    namespace obs = ayther::engine::audio_observation;
    if (!state.complete)
      return obs::AudioHdRestoreCode::incomplete_payload;
    if (state.voices.size() > obs::kAudioHdVoiceLimit ||
        state.pcm_assets.size() > obs::kAudioHdVoiceLimit)
      return obs::AudioHdRestoreCode::resource_limit;
    if (state.max_skew_samples > state.skew_samples ||
        state.started < state.voices.size())
      return obs::AudioHdRestoreCode::invalid_voice;

    try {
      std::unordered_map<uint64_t, HdMixPcm> assets;
      assets.reserve(state.pcm_assets.size());
      size_t total_samples = 0;
      for (const auto &asset : state.pcm_assets) {
        if (asset.identity == 0 || asset.samples.empty() ||
            asset.samples.size() % 2 != 0 ||
            asset.samples.size() >
                obs::kAudioHdVoicePcmByteLimit / sizeof(int16_t) ||
            total_samples > obs::kAudioHdVoicePcmByteLimit / sizeof(int16_t) -
                                asset.samples.size())
          return obs::AudioHdRestoreCode::invalid_pcm;
        total_samples += asset.samples.size();
        auto pcm = std::make_shared<const std::vector<int16_t>>(asset.samples);
        if (!assets.emplace(asset.identity, std::move(pcm)).second)
          return obs::AudioHdRestoreCode::duplicate_value;
      }

      std::unordered_set<uint64_t> occurrences;
      occurrences.reserve(state.voices.size());
      std::vector<Voice> voices;
      voices.reserve(state.voices.size());
      for (const auto &saved : state.voices) {
        const auto asset = assets.find(saved.pcm_identity);
        if (saved.occurrence == 0 || saved.business_key == 0 ||
            asset == assets.end() ||
            !occurrences.insert(saved.occurrence).second)
          return obs::AudioHdRestoreCode::invalid_voice;
        const uint64_t frames = asset->second->size() / 2;
        if (saved.source_position >= frames || !std::isfinite(saved.gain) ||
            saved.gain < 0.0F ||
            saved.source_position >
                static_cast<uint64_t>((std::numeric_limits<size_t>::max)()) ||
            saved.loop_begin >
                static_cast<uint64_t>((std::numeric_limits<size_t>::max)()) ||
            saved.loop_end >
                static_cast<uint64_t>((std::numeric_limits<size_t>::max)()) ||
            (saved.output_end_known && saved.output_end < saved.output_start) ||
            (saved.fade_remaining != 0 &&
             (saved.fade_span == 0 ||
              saved.fade_remaining > saved.fade_span)) ||
            (saved.tail_active && saved.fade_effect_active) ||
            static_cast<std::uint8_t>(saved.category) >
                static_cast<std::uint8_t>(ayther::AudioCategory::voice) ||
            (saved.loop_end != 0 &&
             (saved.loop_begin >= saved.loop_end || saved.loop_end > frames)))
          return obs::AudioHdRestoreCode::invalid_voice;
        Voice voice;
        voice.key = saved.business_key;
        voice.occurrence = saved.occurrence;
        voice.cause_producer = saved.cause_producer;
        voice.cause_sequence = saved.cause_sequence;
        voice.pcm = asset->second;
        voice.state_pcm_identity = saved.pcm_identity;
        voice.start_sample = saved.output_start;
        voice.pos = static_cast<size_t>(saved.source_position);
        voice.output_end = saved.output_end;
        voice.output_end_known = saved.output_end_known;
        voice.gain = saved.gain;
        voice.looping = saved.looping;
        voice.event = saved.event;
        voice.end_frame = saved.end_frame;
        voice.cut_frame = saved.cut_frame;
        voice.fade_left = saved.fade_remaining;
        voice.fade_span = saved.fade_span;
        voice.fade_frames = saved.fade_frames;
        voice.tail_active = saved.tail_active;
        voice.fade_effect_active = saved.fade_effect_active;
        voice.loop_begin = static_cast<size_t>(saved.loop_begin);
        voice.loop_end = static_cast<size_t>(saved.loop_end);
        voice.late_samples = saved.late_samples;
        voice.category = saved.category;
        voices.push_back(std::move(voice));
      }

      voices_ = std::move(voices);
      started_ = state.started;
      mixed_samples_ = state.mixed_samples;
      skew_samples_ = state.skew_samples;
      max_skew_ = state.max_skew_samples;
      return obs::AudioHdRestoreCode::restored;
    } catch (...) {
      return obs::AudioHdRestoreCode::resource_limit;
    }
  }

  // ---- Telemetry (Phase 0) --------------------------------------------
  uint64_t started() const { return started_; }
  uint64_t mixed_samples() const { return mixed_samples_; }
  /// Accumulated lateness samples from voices that reached an already-past
  /// block (a sustained 0 = the placement works; growing = some path is
  /// scheduling against a timeline that was already flushed).
  uint64_t skew_samples() const { return skew_samples_; }
  uint64_t max_skew_samples() const { return max_skew_; }

private:
  std::vector<Voice> voices_;
  void *position_context_ = nullptr;
  PositionCallback position_callback_ = nullptr;
  void *loop_context_ = nullptr;
  LoopCallback loop_callback_ = nullptr;
  void *end_context_ = nullptr;
  EndCallback end_callback_ = nullptr;
  void *effect_context_ = nullptr;
  EffectCallback effect_callback_ = nullptr;
  uint64_t started_ = 0;
  uint64_t mixed_samples_ = 0;
  uint64_t skew_samples_ = 0;
  uint64_t max_skew_ = 0;

  void notify_end(const Voice &voice, EndReason reason,
                  uint64_t output_position, uint64_t frame_position,
                  bool output_known, bool frame_known) noexcept {
    if (!end_callback_ || voice.occurrence == 0)
      return;
    end_callback_(
        end_context_,
        VoiceEnd{{voice.occurrence, voice.cause_producer, voice.cause_sequence},
                 voice.key,
                 reason,
                 voice.pos,
                 voice.pcm ? voice.pcm->size() / 2 : 0,
                 output_position,
                 frame_position,
                 output_known,
                 frame_known});
  }

  void notify_effect(const Voice &voice, EffectKind kind, EffectStage stage,
                     uint64_t source_begin, uint64_t source_end,
                     uint64_t output_begin, uint64_t output_end,
                     uint64_t frame_position, uint32_t remaining_begin,
                     uint32_t remaining_end, bool output_known,
                     bool frame_known) noexcept {
    if (!effect_callback_ || voice.occurrence == 0)
      return;
    effect_callback_(effect_context_,
                     LifetimeEffect{{voice.occurrence, voice.cause_producer,
                                     voice.cause_sequence},
                                    voice.key,
                                    kind,
                                    stage,
                                    source_begin,
                                    source_end,
                                    output_begin,
                                    output_end,
                                    frame_position,
                                    remaining_begin,
                                    remaining_end,
                                    output_known,
                                    frame_known});
  }
};
