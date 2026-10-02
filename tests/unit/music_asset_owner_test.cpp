#include <ayther/engine/music_asset_owner.hpp>

#include <cstdio>

namespace {
void check(bool value, const char *message, int &failures) {
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
  }
}
} // namespace

int main() {
  using namespace ayther::engine;
  int failures = 0;
  const AssetAssignment general{
      AssetAssignmentId{1}, SequenceSegmentId{1}, AssetId{10}, {100, 200}};
  const AssetAssignment individual{
      AssetAssignmentId{2}, SequenceSegmentId{1}, AssetId{20}, {300, 360}};
  const MusicAssetBinding binding{
      general,
      {{SequenceNodeId{2}, individual}, {SequenceNodeId{3}, std::nullopt}}};

  MusicAssetOwner owner;
  const MusicVoiceOwner voice{MusicIdentityId{7}, OccurrenceId{9},
                              AudioBusId{1}};
  auto result = owner.enter(voice, SequenceNodeId{1}, binding, true);
  check(result.action == MusicAssetAction::started &&
            result.assignment == AssetAssignmentId{1} &&
            result.asset == AssetId{10} && result.region.begin == 100 &&
            result.region.end == 200 && owner.voice_count() == 1,
        "general assignment starts one physical voice", failures);

  result = owner.enter(voice, SequenceNodeId{2}, binding, true);
  check(result.action == MusicAssetAction::continued &&
            result.assignment == AssetAssignmentId{2} &&
            result.asset == AssetId{20} && result.region.begin == 300 &&
            result.region.end == 360 && owner.voice_count() == 1,
        "individual assignment changes source without competing bus owner",
        failures);
  owner.update_source_cursor(341);
  check(owner.voice_count() == 1 && owner.source_cursor() == 341,
        "local source cursor change preserves physical music owner", failures);

  const MusicVoiceOwner same_identity_new_occurrence{
      MusicIdentityId{7}, OccurrenceId{10}, AudioBusId{1}};
  result = owner.enter(same_identity_new_occurrence, SequenceNodeId{1}, binding,
                       false);
  check(result.action == MusicAssetAction::kept_active &&
            owner.current_owner() == voice && owner.voice_count() == 1,
        "unsafe entry preserves active occurrence", failures);

  MusicAssetOwner inactive;
  result = inactive.enter(voice, SequenceNodeId{1}, binding, false);
  check(result.action == MusicAssetAction::kept_original &&
            inactive.voice_count() == 0,
        "unsafe inactive entry keeps original audio", failures);
  result = inactive.enter(voice, SequenceNodeId{3}, binding, true);
  check(result.action == MusicAssetAction::invalid_individual &&
            inactive.voice_count() == 0,
        "declared missing individual asset never falls back to general",
        failures);
  return failures == 0 ? 0 : 1;
}
