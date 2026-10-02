#include <ayther/audio_playback_policy.h>

#include <cstdio>
#include <string_view>

namespace {
int failures{};
void check(bool value, std::string_view message) {
  std::printf("%s %.*s\n", value ? "[ok]" : "[FAIL]",
              static_cast<int>(message.size()), message.data());
  failures += value ? 0 : 1;
}
} // namespace

int main() {
  using namespace ayther;

  const auto music = default_audio_playback_policy(AudioCategory::music);
  const auto ambient = default_audio_playback_policy(AudioCategory::ambient);
  const auto effect = default_audio_playback_policy(AudioCategory::effect);
  const auto voice = default_audio_playback_policy(AudioCategory::voice);

  check(valid(music) && music.max_voices == 1,
        "music default is a valid exclusive policy");
  check(valid(ambient) && ambient.max_voices == 1,
        "ambient default is a valid exclusive policy");
  check(valid(effect) && effect.max_voices == 32,
        "effect default permits bounded overlap");
  check(valid(voice) && voice.max_voices == 8,
        "voice default permits bounded distinct keys");

  const auto music_repeat = decide_audio_playback(
      music, {PlaybackState::loop, PlaybackEvent::same_identity_trigger});
  check(music_repeat.action == PlaybackAction::continue_playback &&
            music_repeat.reason == PlaybackReason::same_identity_continuity &&
            music_repeat.next_state == PlaybackState::loop &&
            music_repeat.preserve_occurrence && music_repeat.preserve_cursor,
        "music repetition preserves occurrence and cursor");

  const auto ambient_repeat = decide_audio_playback(
      ambient, {PlaybackState::body, PlaybackEvent::same_identity_trigger});
  check(ambient_repeat.action == PlaybackAction::continue_playback &&
            ambient_repeat.preserve_occurrence &&
            ambient_repeat.preserve_cursor,
        "ambient repetition preserves occurrence and cursor");

  const auto effect_repeat = decide_audio_playback(
      effect, {PlaybackState::body, PlaybackEvent::same_identity_trigger});
  check(effect_repeat.action == PlaybackAction::overlap &&
            !effect_repeat.preserve_occurrence &&
            !effect_repeat.preserve_cursor,
        "effect repetition creates a bounded occurrence");

  const auto voice_repeat = decide_audio_playback(
      voice, {PlaybackState::body, PlaybackEvent::same_identity_trigger});
  check(voice_repeat.action == PlaybackAction::restart &&
            voice_repeat.reason == PlaybackReason::configured_restart,
        "same-key voice restarts by default");

  const auto transition = decide_audio_playback(
      music, {PlaybackState::body, PlaybackEvent::different_identity_trigger});
  check(transition.action == PlaybackAction::replace &&
            transition.reason == PlaybackReason::exclusive_bus_transition &&
            transition.next_state == PlaybackState::body,
        "different music replaces on its exclusive bus");

  const auto paused =
      decide_audio_playback(music, {PlaybackState::loop, PlaybackEvent::pause});
  const auto resumed = decide_audio_playback(
      music,
      {PlaybackState::paused, PlaybackEvent::resume, PlaybackState::loop});
  check(paused.action == PlaybackAction::pause &&
            paused.next_state == PlaybackState::paused &&
            paused.preserve_occurrence && paused.preserve_cursor &&
            resumed.action == PlaybackAction::resume &&
            resumed.next_state == PlaybackState::loop &&
            resumed.preserve_occurrence && resumed.preserve_cursor,
        "pause and resume preserve complete logical continuity");

  const auto ended = decide_audio_playback(
      music, {PlaybackState::body, PlaybackEvent::source_end});
  check(ended.action == PlaybackAction::finish &&
            ended.reason == PlaybackReason::source_exhausted &&
            ended.next_state == PlaybackState::ended,
        "source end is explicitly terminal");

  auto invalid = effect;
  invalid.max_voices = 0;
  check(!valid(invalid), "zero voices is outside the public policy limits");
  invalid.max_voices = 257;
  check(!valid(invalid), "more than 256 voices is outside the public limits");

  // RF-17.8 / QA-283: los tres recorridos sólo adaptan identidad, tiempo y
  // estado. La decisión se toma una única vez en la política común.
  PlaybackDecision common[3]{};
  for (std::size_t i = 0; i < 3; ++i) {
    const PlaybackRoute route = static_cast<PlaybackRoute>(i);
    const auto normalized = normalize_audio_playback(
        {route, music, PlaybackState::loop,
         PlaybackEvent::same_identity_trigger, PlaybackState::inactive,
         0xaabbccdd, 180, 44'100, false, true});
    common[i] = normalized.decision;
    check(normalized.identity == 0xaabbccdd &&
              normalized.timeline_frame == 180 &&
              normalized.source_cursor == 44'100 &&
              !normalized.members_present && normalized.silent,
          "adapter preserves semantic identity, clocks and observable state");
  }
  check(common[0].action == common[1].action &&
            common[1].action == common[2].action &&
            common[0].reason == common[1].reason &&
            common[1].reason == common[2].reason && common[0].preserve_cursor &&
            common[1].preserve_cursor && common[2].preserve_cursor,
        "authoring, preview and runtime/replay share one decision");

  const auto silent_end = normalize_audio_playback(
      {PlaybackRoute::preview, music, PlaybackState::body,
       PlaybackEvent::authored_end, PlaybackState::inactive, 7, 9, 11, false,
       true});
  check(silent_end.decision.reason == PlaybackReason::authored_window_exhausted,
        "silence and absent members do not invent a different end reason");

  std::printf("%s\n", failures ? "FAIL" : "OK");
  return failures ? 1 : 0;
}
