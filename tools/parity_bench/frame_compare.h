// parity_bench compare (spec 008, TP-005.16, D-3): first divergence per
// subplane between two dumps written by frame_dump.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ayther::parity {

struct Divergence {
  std::string subplane;
  std::uint32_t frame = 0; ///< 1-based frame of the first difference
  std::string a_hash, b_hash;
};

struct CompareResult {
  bool ok = false; ///< false: a dump is malformed or lengths differ
  std::string error;
  std::vector<Divergence> divergences; ///< one per diverging subplane
};

/// `exceptions`: subplane names that are not compared (the allowed list).
CompareResult compare_dumps(const std::string &a, const std::string &b,
                            const std::vector<std::string> &exceptions);

} // namespace ayther::parity
