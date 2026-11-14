#include "identities.h"

#include <ayther/ayther_core_ffi.h>
#include <ayther/ayther_session.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace ayther::parity {
namespace {

constexpr std::uint32_t kPaletteStableFrames = 30; // runtime latch

bool parse_u32(std::string_view s, std::uint32_t &out) {
  if (s.empty())
    return false;
  const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
  return ec == std::errc{} && p == s.data() + s.size();
}

bool parse_u64(std::string_view s, std::uint64_t &out, int base = 10) {
  if (s.empty())
    return false;
  const auto [p, ec] =
      std::from_chars(s.data(), s.data() + s.size(), out, base);
  return ec == std::errc{} && p == s.data() + s.size();
}

std::string hex16(std::uint64_t v) {
  char buf[17];
  std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
  return buf;
}

SegmentIds empty_segment(Segment range) {
  SegmentIds s;
  s.range = range;
  for (const char *name : kIdentityClasses)
    s.classes[name];
  return s;
}

} // namespace

std::optional<std::vector<Segment>> parse_segments(std::string_view text) {
  std::vector<Segment> out;
  while (!text.empty()) {
    const auto comma = text.find(',');
    const std::string_view item = text.substr(0, comma);
    const auto dash = item.find('-');
    Segment s;
    if (dash == std::string_view::npos ||
        !parse_u32(item.substr(0, dash), s.first) ||
        !parse_u32(item.substr(dash + 1), s.last) || s.first > s.last)
      return std::nullopt;
    out.push_back(s);
    if (comma == std::string_view::npos)
      break;
    text.remove_prefix(comma + 1);
    if (text.empty())
      return std::nullopt; // trailing comma
  }
  if (out.empty())
    return std::nullopt;
  return out;
}

struct IdentityCollector::Impl {
  std::vector<SegmentIds> segments;
  AytherAudioEventDetector *audio = nullptr;
  std::uint32_t frames = 0;
  std::array<std::uint64_t, 4> line_sig{};
  std::array<std::uint32_t, 4> line_run{};

  ~Impl() {
    if (audio != nullptr)
      ayther_audio_event_free(audio);
  }

  // Every segment that holds `frame` (segments may overlap).
  template <class F> void each(std::uint32_t frame, F f) {
    for (auto &s : segments)
      if (frame >= s.range.first && frame <= s.range.last)
        f(s);
  }

  static void add(SegmentIds &s, const char *cls, std::uint64_t id) {
    auto &c = s.classes[cls];
    ++c.occurrences;
    c.ids.insert(id);
  }
};

IdentityCollector::IdentityCollector(std::vector<Segment> segments)
    : impl_(std::make_unique<Impl>()) {
  for (const auto &s : segments)
    impl_->segments.push_back(empty_segment(s));
  impl_->audio = ayther_audio_event_new();
  ayther_audio_event_set_pal(impl_->audio, 0);
}

IdentityCollector::~IdentityCollector() = default;

void IdentityCollector::frame(std::uint32_t frame, const FrameView &v,
                              const std::uint8_t *fallback_cram,
                              std::size_t fallback_size) {
  auto &im = *impl_;
  im.frames = frame;
  ayther_audio_event_process_frame(im.audio, frame, v.chip_writes,
                                   v.chip_write_count);

  // Palette variants: a line enters the set once its 16 colours have been the
  // same for kPaletteStableFrames frames in a row (one entry per stable run).
  std::uint16_t words[64] = {};
  const std::uint8_t *cram = v.scene_cram;
  std::size_t cram_size = v.scene_cram_size;
  if (cram == nullptr || cram_size < sizeof words) {
    cram = fallback_cram;
    cram_size = fallback_size;
  }
  const bool have_cram = cram != nullptr && cram_size >= sizeof words;
  if (have_cram)
    std::memcpy(words, cram, sizeof words);
  for (std::uint8_t line = 0; line < 4; ++line) {
    if (!have_cram) {
      im.line_run[line] = 0;
      continue;
    }
    const std::uint64_t sig = ayther_palette_signature(words, 64, line, 0xFFFF);
    if (sig != im.line_sig[line] || im.line_run[line] == 0) {
      im.line_sig[line] = sig;
      im.line_run[line] = 1;
    } else if (im.line_run[line] < kPaletteStableFrames) {
      ++im.line_run[line];
    }
    if (im.line_run[line] == kPaletteStableFrames) {
      ++im.line_run[line]; // latched: once per stable run
      im.each(frame,
              [&](SegmentIds &s) { Impl::add(s, "palette_variant", sig); });
    }
  }

  im.each(frame, [&](SegmentIds &s) {
    for (std::uint32_t i = 0;
         v.sprite_occs != nullptr && i < v.sprite_occ_count; ++i) {
      Impl::add(s, "sprite", v.sprite_occs[i].hash);
      if (v.sprite_occs[i].anim_group_id != 0)
        Impl::add(s, "anim_group", v.sprite_occs[i].anim_group_id);
    }
    for (std::uint32_t i = 0; v.tile_occs != nullptr && i < v.tile_occ_count;
         ++i)
      Impl::add(s, "tile", v.tile_occs[i].hash);
    for (std::uint32_t i = 0;
         v.plane_cells != nullptr && i < v.plane_cell_count; ++i)
      Impl::add(s, "plane_tile", v.plane_cells[i].hash);
  });
}

Identities IdentityCollector::finish() {
  auto &im = *impl_;
  ayther_audio_event_finish(im.audio);
  const std::uint32_t n = ayther_audio_event_count(im.audio);
  std::vector<AytherAudioEvent> events(n);
  if (n != 0)
    events.resize(ayther_audio_event_get(im.audio, events.data(), n));
  for (const auto &e : events) {
    const char *cls = e.chip == 0   ? "audio_fm"
                      : e.chip == 1 ? "audio_psg"
                                    : nullptr;
    im.each(e.start_frame, [&](SegmentIds &s) {
      if (cls != nullptr)
        Impl::add(s, cls, e.signature);
      Impl::add(s, "instrument", e.instrument);
    });
  }
  Identities out;
  out.frames = im.frames;
  out.segments = std::move(im.segments);
  return out;
}

std::string format_identities(const Identities &ids) {
  std::string out = "parity_bench identities 1\n";
  out += "frames " + std::to_string(ids.frames) + "\n";
  for (const auto &s : ids.segments) {
    out += "segment " + std::to_string(s.range.first) + " " +
           std::to_string(s.range.last) + "\n";
    for (const char *name : kIdentityClasses) {
      const auto it = s.classes.find(name);
      const ClassIds empty;
      const ClassIds &c = it == s.classes.end() ? empty : it->second;
      out += std::string("class ") + name + " occurrences " +
             std::to_string(c.occurrences) + " distinct " +
             std::to_string(c.ids.size()) + "\n";
      for (const auto id : c.ids)
        out += "id " + hex16(id) + "\n";
    }
  }
  return out;
}

std::optional<Identities> parse_identities(std::string_view text,
                                           std::string *error) {
  const auto fail = [&](std::size_t line, const char *what) {
    if (error != nullptr)
      *error = "line " + std::to_string(line) + ": " + what;
    return std::nullopt;
  };
  Identities out;
  std::istringstream in{std::string(text)};
  std::string line;
  std::size_t no = 0;
  ClassIds *current = nullptr;
  std::uint64_t expect = 0;
  bool header = false, frames = false;
  while (std::getline(in, line)) {
    ++no;
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    std::istringstream f(line);
    std::string key, a, b, c, d;
    f >> key;
    if (!header) {
      f >> a >> b;
      if (key != "parity_bench" || a != "identities" || b != "1")
        return fail(no, "not a parity_bench identities 1 file");
      header = true;
      continue;
    }
    if (key == "frames" && !frames) {
      f >> a;
      if (!parse_u32(a, out.frames))
        return fail(no, "bad frames");
      frames = true;
    } else if (key == "segment") {
      if (current != nullptr && current->ids.size() != expect)
        return fail(no, "id count does not match distinct");
      current = nullptr;
      Segment s;
      f >> a >> b;
      if (!parse_u32(a, s.first) || !parse_u32(b, s.last) || s.first > s.last)
        return fail(no, "bad segment");
      out.segments.push_back(empty_segment(s));
    } else if (key == "class") {
      if (out.segments.empty())
        return fail(no, "class before segment");
      if (current != nullptr && current->ids.size() != expect)
        return fail(no, "id count does not match distinct");
      std::string name;
      f >> name >> a >> b >> c >> d;
      auto &classes = out.segments.back().classes;
      const auto it = classes.find(name);
      if (it == classes.end() || a != "occurrences" || c != "distinct" ||
          !parse_u64(b, it->second.occurrences) || !parse_u64(d, expect))
        return fail(no, "bad class line");
      current = &it->second;
    } else if (key == "id") {
      std::uint64_t id = 0;
      f >> a;
      if (current == nullptr || a.size() != 16 || !parse_u64(a, id, 16) ||
          !current->ids.insert(id).second)
        return fail(no, "bad or repeated id");
    } else if (!key.empty()) {
      return fail(no, "unknown record");
    }
  }
  if (!header || !frames)
    return fail(no, "truncated file");
  if (current != nullptr && current->ids.size() != expect)
    return fail(no, "id count does not match distinct");
  return out;
}

bool IdentityComparison::equal() const {
  if (!ok)
    return false;
  for (const auto &s : segments)
    for (const auto &c : s.classes)
      if (c.missing != 0 || c.extra != 0)
        return false;
  return true;
}

IdentityComparison compare_identities(const Identities &golden,
                                      const Identities &candidate) {
  IdentityComparison cmp;
  if (golden.segments.size() != candidate.segments.size()) {
    cmp.error = "the files cover different segments";
    return cmp;
  }
  for (std::size_t i = 0; i < golden.segments.size(); ++i) {
    const auto &g = golden.segments[i];
    const auto &k = candidate.segments[i];
    if (g.range.first != k.range.first || g.range.last != k.range.last) {
      cmp.error = "the files cover different segments";
      return cmp;
    }
    SegmentDiff sd;
    sd.range = g.range;
    for (const char *name : kIdentityClasses) {
      const ClassIds &a = g.classes.at(name);
      const ClassIds &b = k.classes.at(name);
      ClassDiff d;
      d.name = name;
      d.golden = a.ids.size();
      d.candidate = b.ids.size();
      d.golden_occurrences = a.occurrences;
      d.candidate_occurrences = b.occurrences;
      std::vector<std::uint64_t> miss, extra;
      std::set_difference(a.ids.begin(), a.ids.end(), b.ids.begin(),
                          b.ids.end(), std::back_inserter(miss));
      std::set_difference(b.ids.begin(), b.ids.end(), a.ids.begin(),
                          a.ids.end(), std::back_inserter(extra));
      d.missing = miss.size();
      d.extra = extra.size();
      d.missing_ids.assign(
          miss.begin(), miss.begin() + std::min<std::size_t>(8, miss.size()));
      d.extra_ids.assign(extra.begin(), extra.begin() + std::min<std::size_t>(
                                                            8, extra.size()));
      std::uint64_t h = 1469598103934665603ULL;
      for (const auto id : a.ids)
        for (int byte = 0; byte < 8; ++byte)
          h = (h ^ ((id >> (8 * byte)) & 0xFF)) * 1099511628211ULL;
      d.set_hash = h;
      sd.classes.push_back(std::move(d));
    }
    cmp.segments.push_back(std::move(sd));
  }
  cmp.ok = true;
  return cmp;
}

std::string comparison_json(const IdentityComparison &cmp) {
  const auto ids = [](const std::vector<std::uint64_t> &v) {
    std::string s = "[";
    for (std::size_t i = 0; i < v.size(); ++i)
      s += (i ? ",\"" : "\"") + hex16(v[i]) + "\"";
    return s + "]";
  };
  std::string out = "{\"equal\":";
  out += cmp.equal() ? "true" : "false";
  out += ",\"segments\":[";
  for (std::size_t i = 0; i < cmp.segments.size(); ++i) {
    const auto &s = cmp.segments[i];
    out += i ? "," : "";
    out += "{\"range\":[" + std::to_string(s.range.first) + "," +
           std::to_string(s.range.last) + "],\"classes\":{";
    for (std::size_t j = 0; j < s.classes.size(); ++j) {
      const auto &c = s.classes[j];
      out += j ? "," : "";
      out += "\"" + c.name + "\":{\"distinct\":[" + std::to_string(c.golden) +
             "," + std::to_string(c.candidate) + "],\"occurrences\":[" +
             std::to_string(c.golden_occurrences) + "," +
             std::to_string(c.candidate_occurrences) +
             "],\"missing\":" + std::to_string(c.missing) +
             ",\"extra\":" + std::to_string(c.extra);
      if (c.missing == 0 && c.extra == 0)
        out += ",\"set_fnv64\":\"" + hex16(c.set_hash) + "\"";
      else
        out += ",\"missing_ids\":" + ids(c.missing_ids) +
               ",\"extra_ids\":" + ids(c.extra_ids);
      out += "}";
    }
    out += "}}";
  }
  out += "]}";
  return out;
}

} // namespace ayther::parity
