#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace qa_selection {
struct Fact {
  const char *kind{};
  std::uint64_t signature{};
  std::uint64_t best{};
  int rank{};
};
std::array<Fact, 32> facts{};
std::size_t count{};
bool overflow{};
bool lose{};
void record(const char *kind, std::uint64_t signature, std::uint64_t best,
            int rank) noexcept {
  if (lose && std::string_view(kind) == "candidate" && signature == 20) {
    return;
  }
  if (count == facts.size()) {
    overflow = true;
    return;
  }
  facts[count++] = {kind, signature, best, rank};
}
void require(bool valid, const char *message) {
  if (!valid) {
    throw std::runtime_error(message);
  }
}
} // namespace qa_selection

#if QA_SELECTION_OBSERVER
#include <observed_match.h>
#else
#include <audio_match_rule.h>
#endif

struct SessionFixture {
  std::unordered_set<std::uint64_t> audio_event_assign;
  ayther::AudioMatchIndex audio_match_index;
#if QA_SELECTION_OBSERVER
#include <observed_resolve.inc>
#else
#include <reference_resolve.inc>
#endif
};

int main(int argc, char *argv[]) {
  using qa_selection::require;
  try {
    require(argc == 3, "invalid_arguments");
    const std::filesystem::path directory{argv[1]};
    const std::string_view mode{argv[2]};
    require(mode == "match" || mode == "miss" || mode == "exact" ||
                mode == "lost",
            "invalid_case");
    for (const char *name :
         {"state.toml", "input.toml", "decision.toml", "facts.toml"}) {
      require(!std::filesystem::exists(directory / name), "output_exists");
    }
    qa_selection::lose = mode == "lost";
    SessionFixture session;
    session.audio_event_assign.insert(99);
    struct Assignment {
      std::uint64_t signature;
      std::uint8_t pitch;
    };
    constexpr std::array<Assignment, 3> assignments{
        {{10, 63}, {20, 64}, {15, 64}}};
    for (const auto &entry : assignments) {
      session.audio_match_index.add(entry.signature,
                                    ayther::AudioMatchRule::kInstrumentPitch, 7,
                                    entry.pitch);
    }
    const std::uint64_t signature = mode == "exact" ? 99 : 1000;
    const std::uint8_t pitch = mode == "miss" ? 62 : 64;
    std::uint64_t selected = UINT64_MAX;
    const bool matched =
        session.resolve_event_sig(signature, 7, pitch, &selected);
    const std::uint64_t expected = mode == "exact"  ? 99
                                   : mode == "miss" ? UINT64_MAX
                                                    : 15;
    require(matched == (mode != "miss") && selected == expected,
            "reference_decision_differs");
    std::unordered_set<std::uint64_t> candidates;
    std::size_t pitch_rejected = 0;
    std::size_t terminals = 0;
    bool intact = !qa_selection::overflow;
    for (std::size_t i = 0; i < qa_selection::count; ++i) {
      const auto &fact = qa_selection::facts[i];
      const std::string_view kind{fact.kind};
      if (kind == "candidate") {
        intact = candidates.insert(fact.signature).second && intact;
      } else if (kind == "pitch_rejected" || kind == "winner_updated" ||
                 kind == "lower_priority") {
        intact = candidates.contains(fact.signature) && intact;
        pitch_rejected += kind == "pitch_rejected" ? 1 : 0;
      } else {
        ++terminals;
        intact = intact && i + 1 == qa_selection::count &&
                 ((mode == "miss" && kind == "no_match") ||
                  (mode == "exact" && kind == "exact_selected" &&
                   fact.signature == selected) ||
                  ((mode == "match" || mode == "lost") && kind == "selected" &&
                   fact.signature == selected));
      }
    }
    intact =
        intact && terminals == 1 &&
        candidates == (mode == "exact"
                           ? std::unordered_set<std::uint64_t>{}
                           : std::unordered_set<std::uint64_t>{10, 15, 20}) &&
        pitch_rejected == (mode == "exact"  ? 0U
                           : mode == "miss" ? 3U
                                            : 1U);
    std::ofstream file;
    file.exceptions(std::ios::failbit | std::ios::badbit);
    file.open(directory / "state.toml");
    file << "schema_version = 1\nexact_assignments = [99]\n";
    for (const auto &entry : assignments) {
      file << "[[assignments]]\nsignature = " << entry.signature
           << "\ninstrument = 7\nrule = 2\npitch = "
           << static_cast<unsigned>(entry.pitch) << '\n';
    }
    file.close();
    file.open(directory / "input.toml");
    file << "schema_version = 1\nevent_id = 1\nsource = "
            "\"synthetic_active_event\"\nsignature = "
         << signature
         << "\ninstrument = 7\npitch = " << static_cast<unsigned>(pitch)
         << '\n';
    file.close();
    file.open(directory / "decision.toml");
    file << "schema_version = 1\nevent_id = 1\nmatched = "
         << (matched ? "true" : "false") << "\nselected = \"" << selected
         << "\"\n";
    file.close();
    file.open(directory / "facts.toml");
    file << "schema_version = 1\nobserver_enabled = "
         << (QA_SELECTION_OBSERVER ? "true" : "false")
         << "\ntrace_complete = " << (intact ? "true" : "false") << '\n';
    for (std::size_t i = 0; i < qa_selection::count; ++i) {
      const auto &fact = qa_selection::facts[i];
      file << "\n[[facts]]\nevent_id = 1\nsequence = " << i << "\nkind = \""
           << fact.kind << "\"\nsignature = " << fact.signature
           << "\nbest_signature = " << fact.best << "\nrank = " << fact.rank
           << '\n';
    }
    file.close();
    if (QA_SELECTION_OBSERVER && !intact) {
      std::fputs("selection_trace_incomplete\n", stderr);
      return 2;
    }
    require(QA_SELECTION_OBSERVER || qa_selection::count == 0,
            "reference_instrumented");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "selection_probe_failed: %s\n", error.what());
    return 1;
  }
}
