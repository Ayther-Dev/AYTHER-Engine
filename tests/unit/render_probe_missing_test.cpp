// Spec 002, BR-093 (RF-2.11, RF-9.3): replay QA stops on an assigned asset
// that cannot be read. The probe's rule: the first replacement drawn this
// frame with a failed texture is a missing_asset; one that was not drawn
// (hidden, or HD off) and textures that are ready or pending are not.
#include "../../tools/render_probe/probe_missing.h"

#include <cstdio>
#include <cstring>
#include <exception>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}

namespace ro = ayther::engine::render_observation;

AytherSpriteSub sub(const char *asset) {
  AytherSpriteSub s{};
  std::snprintf(s.asset_path, sizeof(s.asset_path), "%s", asset);
  return s;
}
} // namespace

int main() try {
  const std::vector<AytherSpriteSub> subs = {sub("ok"), sub("hidden_bad"),
                                             sub("bad"), sub("later_bad")};
  std::vector<ro::ReplacementDraw> rows = {
      {ro::DrawOutcome::in_pass, ro::TextureState::ready},
      {ro::DrawOutcome::discarded, ro::TextureState::failed},
      {ro::DrawOutcome::in_pass, ro::TextureState::failed},
      {ro::DrawOutcome::lane, ro::TextureState::failed}};
  ro::DrawReport report;
  report.hd_enabled = true;
  report.replacements = rows;

  const auto missing = ayther::probe::first_missing_asset(report, subs);
  check(missing.has_value() && missing->replacement == 2 &&
            missing->asset == "bad",
        "RF-2.11: the first drawn replacement with a failed texture is a "
        "missing_asset");

  rows[2].texture = ro::TextureState::pending;
  rows[3].texture = ro::TextureState::ready;
  report.replacements = rows;
  check(!ayther::probe::first_missing_asset(report, subs).has_value(),
        "a pending texture or one not drawn (hidden) is not a missing_asset");

  report.replacements = {};
  check(!ayther::probe::first_missing_asset(report, subs).has_value(),
        "an empty report has no missing_asset");

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
