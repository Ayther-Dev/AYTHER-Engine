// parity_bench check (spec 008, TP-209.1, D-3, RF-10.5): strict exception
// accounting of plane 3. A plain-text file declares every divergence the bench
// may see as a tuple `<rom> <frame> <subplane> <EXC-nnn>`; the check fails when
// a divergence is not declared AND when a declared tuple is no longer produced,
// so an exception cannot outlive the defect it describes.
#pragma once

#include "frame_compare.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace ayther::parity {

/// One line of the exception file. `rom` and the frame range accept `*`
/// (every catalogued ROM, every dumped frame); the range is inclusive.
struct ExceptionEntry {
  std::string rom; ///< ROM name, or "*"
  bool any_frame = false;
  std::uint32_t first = 0, last = 0;
  std::string subplane, id;
  int line = 0; ///< 1-based line in the file
};

struct ExceptionFile {
  bool ok = false;
  std::string error;
  std::vector<ExceptionEntry> entries;
};

/// Grammar: one tuple per line, four fields separated by blanks; `#` starts a
/// comment. Frame is `*`, `N` or `N-M` (1 <= N <= M); the subplane is one of
/// kSubplaneNames; the id is `EXC-` followed by three digits.
ExceptionFile parse_exception_file(const std::string &text);

/// Ids of the rows of docs/parity-exceptions.md (lines starting `| EXC-`).
std::set<std::string> registry_ids(const std::string &markdown);

/// The divergences between the two dumps of one catalogued ROM.
struct RomDiff {
  std::string rom;
  std::vector<std::uint32_t> frames;
  std::vector<Divergence> divergences;
};

struct Tuple {
  std::string rom;
  std::uint32_t frame = 0;
  std::string subplane;
  std::string id; ///< declaring exception; empty when undeclared
};

struct AccountResult {
  bool ok = false; ///< false: the file contradicts itself or the catalog
  std::string error;
  std::vector<Tuple> undeclared; ///< divergences no tuple declares
  std::vector<Tuple> unproduced; ///< declared tuples that did not diverge
  std::map<std::string, std::size_t> declared; ///< produced tuples per id
  /// ROMs whose divergences are all declared by `*` ROM entries.
  std::size_t uniform_roms = 0;
  bool clean() const { return ok && undeclared.empty() && unproduced.empty(); }
};

/// `known_ids`: when not empty, every id of the file must be in it.
AccountResult account(const ExceptionFile &file,
                      const std::vector<RomDiff> &roms,
                      const std::set<std::string> &known_ids);

} // namespace ayther::parity
