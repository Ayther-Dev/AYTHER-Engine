#include <ayther/engine/music_pack_contract.hpp>

#include <cstdio>
#include <string>
#include <vector>

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
  const std::string packed = "ayther.music-contract.v1\n"
                             "profile rf18-v1\n"
                             "requires music-sequence-v1\n"
                             "source-rate-hz 48000\n"
                             "identity 9\n"
                             "name The Battle\n"
                             "bus 1\n"
                             "entry 1\n"
                             "segment 1 intro\n"
                             "segment 2 loop\n"
                             "node 1 segment 1 assignment 7\n"
                             "asset 5 region 0 48000 assignment 7\n"
                             "role signature:entry\n"
                             "accepted accepted-proposal-7\n";
  const auto parsed = read_music_pack_contract(packed, {"music-sequence-v1"});
  check(parsed.status == MusicPackContractStatus::accepted &&
            parsed.profile == "rf18-v1" && parsed.source_rate_hz == 48'000,
        "new contract validates profile, capability and source rate", failures);
  check(parsed.identities.size() == 1 && parsed.segments.size() == 2 &&
            parsed.assignments.size() == 1 &&
            parsed.assignments[0].region.begin == 0 &&
            parsed.assignments[0].region.end == 48'000,
        "identity, segment and assignment inventories stay separate", failures);

  const auto unsupported =
      read_music_pack_contract(packed, {"other-capability"});
  check(unsupported.status == MusicPackContractStatus::unsupported_capability &&
            unsupported.identities.empty(),
        "unknown mandatory capability is rejected without reinterpretation",
        failures);
  const auto truncated = read_music_pack_contract(
      "ayther.music-contract.v1\nprofile rf18-v1\nrequires "
      "music-sequence-v1\nsource-rate-hz 48000\nidentity 9\nsegment",
      {"music-sequence-v1"});
  check(truncated.status == MusicPackContractStatus::invalid_format &&
            truncated.identities.empty(),
        "interrupted migration is rejected atomically", failures);
  const auto legacy = read_music_pack_contract(
      "ayther.music-contract.v0\nidentity 4\nsegment 8 body\n",
      {"music-sequence-v1"});
  check(legacy.status == MusicPackContractStatus::legacy_read_only &&
            legacy.identities.size() == 1 && legacy.segments.size() == 1,
        "previous complete format remains readable without new semantics",
        failures);
  return failures == 0 ? 0 : 1;
}
