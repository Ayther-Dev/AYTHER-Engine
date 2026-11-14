#include "exception_accounting.h"

#include "frame_dump.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <sstream>
#include <tuple>

namespace ayther::parity {
namespace {

bool parse_number(const std::string &text, std::uint32_t &out) {
  if (text.empty() || text.size() > 9 ||
      !std::all_of(text.begin(), text.end(),
                   [](unsigned char c) { return std::isdigit(c) != 0; }))
    return false;
  out = static_cast<std::uint32_t>(std::stoul(text));
  return out != 0 && out <= 1000000; // dump --frames accepts 1..1000000
}

bool valid_rom(const std::string &rom) {
  return rom == "*" ||
         (!rom.empty() &&
          std::all_of(rom.begin(), rom.end(), [](unsigned char c) {
            return std::isalnum(c) != 0 || c == '_' || c == '-' || c == '.';
          }));
}

bool valid_id(const std::string &id) {
  return id.size() == 7 && id.compare(0, 4, "EXC-") == 0 &&
         std::all_of(id.begin() + 4, id.end(),
                     [](unsigned char c) { return std::isdigit(c) != 0; });
}

bool known_subplane(const std::string &name) {
  return std::any_of(std::begin(kSubplaneNames), std::end(kSubplaneNames),
                     [&](const char *s) { return name == s; });
}

using Key = std::tuple<std::string, std::uint32_t, std::string>;

} // namespace

ExceptionFile parse_exception_file(const std::string &text) {
  ExceptionFile file;
  std::istringstream in(text);
  std::string raw;
  int number = 0;
  const auto fail = [&](const std::string &why) {
    file.error = "line " + std::to_string(number) + ": " + why;
    file.entries.clear();
    return file;
  };
  while (std::getline(in, raw)) {
    ++number;
    if (const auto hash = raw.find('#'); hash != std::string::npos)
      raw.erase(hash);
    std::istringstream fields(raw);
    std::vector<std::string> f{std::istream_iterator<std::string>(fields),
                               std::istream_iterator<std::string>()};
    if (f.empty())
      continue;
    if (f.size() != 4)
      return fail("expected <rom> <frame> <subplane> <EXC-nnn>");
    ExceptionEntry e;
    e.line = number;
    e.rom = f[0];
    if (!valid_rom(e.rom))
      return fail("invalid rom name: " + e.rom);
    if (f[1] == "*") {
      e.any_frame = true;
    } else if (const auto dash = f[1].find('-'); dash == std::string::npos) {
      if (!parse_number(f[1], e.first))
        return fail("invalid frame: " + f[1]);
      e.last = e.first;
    } else if (!parse_number(f[1].substr(0, dash), e.first) ||
               !parse_number(f[1].substr(dash + 1), e.last) ||
               e.last < e.first) {
      return fail("invalid frame range: " + f[1]);
    }
    e.subplane = f[2];
    if (!known_subplane(e.subplane))
      return fail("unknown subplane: " + e.subplane);
    e.id = f[3];
    if (!valid_id(e.id))
      return fail("invalid exception id: " + e.id);
    file.entries.push_back(std::move(e));
  }
  file.ok = true;
  return file;
}

std::set<std::string> registry_ids(const std::string &markdown) {
  std::set<std::string> ids;
  std::istringstream in(markdown);
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("| EXC-", 0) != 0)
      continue;
    const auto end = line.find('|', 2);
    std::string id =
        line.substr(2, end == std::string::npos ? std::string::npos : end - 2);
    while (!id.empty() && (id.back() == ' ' || id.back() == '\t'))
      id.pop_back();
    ids.insert(id);
  }
  return ids;
}

AccountResult account(const ExceptionFile &file,
                      const std::vector<RomDiff> &roms,
                      const std::set<std::string> &known_ids) {
  AccountResult result;
  if (!file.ok) {
    result.error = file.error;
    return result;
  }
  std::set<std::string> catalog;
  for (const auto &r : roms)
    catalog.insert(r.rom);
  for (const auto &e : file.entries) {
    if (e.rom != "*" && catalog.count(e.rom) == 0) {
      result.error = "line " + std::to_string(e.line) + ": rom " + e.rom +
                     " is not in the catalog";
      return result;
    }
    if (!known_ids.empty() && known_ids.count(e.id) == 0) {
      result.error = "line " + std::to_string(e.line) + ": " + e.id +
                     " is not a row of the exception registry";
      return result;
    }
  }

  // Expand every entry to the tuples it declares; one tuple, one entry.
  std::map<Key, const ExceptionEntry *> expected;
  for (const auto &r : roms) {
    const std::set<std::uint32_t> dumped(r.frames.begin(), r.frames.end());
    for (const auto &e : file.entries) {
      if (e.rom != "*" && e.rom != r.rom)
        continue;
      std::vector<std::uint32_t> frames;
      if (e.any_frame)
        frames.assign(dumped.begin(), dumped.end());
      else
        for (std::uint32_t f = e.first; f <= e.last; ++f)
          frames.push_back(f); // a frame outside the dump is never produced
      for (const auto f : frames) {
        const auto [it, inserted] =
            expected.emplace(Key{r.rom, f, e.subplane}, &e);
        if (!inserted) {
          result.error = "lines " + std::to_string(it->second->line) + " and " +
                         std::to_string(e.line) + " both declare " + r.rom +
                         " " + std::to_string(f) + " " + e.subplane;
          return result;
        }
      }
    }
  }

  std::set<Key> produced;
  for (const auto &r : roms) {
    bool uniform = true;
    for (const auto &d : r.divergences) {
      const Key key{r.rom, d.frame, d.subplane};
      const auto it = expected.find(key);
      if (it == expected.end()) {
        result.undeclared.push_back({r.rom, d.frame, d.subplane, {}});
        uniform = false;
        continue;
      }
      produced.insert(key);
      ++result.declared[it->second->id];
      if (it->second->rom != "*")
        uniform = false;
    }
    if (uniform)
      ++result.uniform_roms;
  }
  for (const auto &[key, entry] : expected)
    if (produced.count(key) == 0)
      result.unproduced.push_back(
          {std::get<0>(key), std::get<1>(key), std::get<2>(key), entry->id});
  result.ok = true;
  return result;
}

} // namespace ayther::parity
