// The Tessera package does not install the ClownMDEmu reference backend (AGPL,
// only built by the windows-clown-reference preset, never installed). The
// tests that run a real Mega Drive ROM need it, so they report SKIPPED (CTest
// SKIP_RETURN_CODE 77) when the linked package lacks it, instead of failing.
#pragma once

#include <ayther/ayther_session.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace ayther::tests {
inline constexpr int kSkipExitCode = 77;

inline bool tessera_clown_backend_available() {
  std::vector<ayther::engine::NativeBackendInfo> backends;
  if (ayther::engine::NativeEmulation::backends(backends).code != 0)
    return false;
  return std::ranges::any_of(
      backends, [](const auto &backend) { return backend.id == "clownmdemu"; });
}

// Prints the reason and returns true when the test must be skipped.
inline bool skip_without_tessera_clown_backend() {
  if (tessera_clown_backend_available())
    return false;
  std::printf("SKIPPED: the linked Tessera package has no 'clownmdemu' "
              "backend (reference builds only, TP-016)\n");
  return true;
}
} // namespace ayther::tests
