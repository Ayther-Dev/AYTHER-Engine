#include "frame_compare.h"

#include "frame_dump.h"

#include <algorithm>
#include <map>
#include <sstream>

namespace ayther::parity {
namespace {

struct Line {
  std::uint32_t frame = 0;
  std::map<std::string, std::string> hash; // subplane -> "hash:count"
};

// Parses one line written by frame_dump; false when it does not have the shape.
bool parse_line(const std::string &text, Line &out) {
  const std::string head = "{\"frame\":";
  if (text.compare(0, head.size(), head) != 0)
    return false;
  std::size_t at = head.size();
  const auto comma = text.find(',', at);
  if (comma == std::string::npos)
    return false;
  try {
    out.frame =
        static_cast<std::uint32_t>(std::stoul(text.substr(at, comma - at)));
  } catch (...) {
    return false;
  }
  for (const char *name : kSubplaneNames) {
    const std::string key = std::string("\"") + name + "\":{\"hash\":\"";
    const auto k = text.find(key);
    if (k == std::string::npos)
      return false;
    const auto end = text.find('}', k);
    if (end == std::string::npos)
      return false;
    // value = hash and count, both compared, kept as "<hash>/<count>"
    const auto v = k + key.size();
    const auto quote = text.find('"', v);
    const std::string count_key = "\"count\":";
    const auto c = text.find(count_key, v);
    if (quote == std::string::npos || quote > end || c == std::string::npos ||
        c > end)
      return false;
    out.hash[name] =
        text.substr(v, quote - v) + "/" +
        text.substr(c + count_key.size(), end - c - count_key.size());
  }
  return true;
}

bool parse(const std::string &dump, std::vector<Line> &lines) {
  std::istringstream in(dump);
  std::string text;
  while (std::getline(in, text)) {
    Line line;
    if (!parse_line(text, line))
      return false;
    lines.push_back(std::move(line));
  }
  return !lines.empty();
}

} // namespace

CompareResult compare_dumps(const std::string &a, const std::string &b,
                            const std::vector<std::string> &exceptions) {
  CompareResult result;
  std::vector<Line> la, lb;
  if (!parse(a, la) || !parse(b, lb)) {
    result.error = "malformed dump";
    return result;
  }
  if (la.size() != lb.size()) {
    result.error = "dumps have a different number of frames";
    return result;
  }
  for (const char *name : kSubplaneNames) {
    if (std::find(exceptions.begin(), exceptions.end(), name) !=
        exceptions.end())
      continue;
    for (std::size_t i = 0; i < la.size(); ++i) {
      if (la[i].frame != lb[i].frame) {
        result.error = "dumps have different frame numbering";
        result.divergences.clear();
        return result;
      }
      if (la[i].hash.at(name) != lb[i].hash.at(name)) {
        result.divergences.push_back(
            {name, la[i].frame, la[i].hash.at(name), lb[i].hash.at(name)});
        break;
      }
    }
  }
  result.ok = true;
  return result;
}

} // namespace ayther::parity
