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
  std::vector<std::uint32_t> frames;   ///< frame numbers of the dumps
};

/// `exceptions`: subplane names that are not compared (the allowed list).
CompareResult compare_dumps(const std::string &a, const std::string &b,
                            const std::vector<std::string> &exceptions);

/// Every differing (frame, subplane), frame-major in kSubplaneNames order,
/// with no exception list: the input of strict exception accounting.
CompareResult diff_dumps(const std::string &a, const std::string &b);

} // namespace ayther::parity
