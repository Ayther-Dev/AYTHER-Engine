#include "trace_arp.h"

#include <ayther/ayther_recording.h>

#include <algorithm>
#include <cstring>
#include <fstream>

namespace ayther::parity {
namespace {

// RetroPad bit for each control: up down left right A B C start.
constexpr std::array<unsigned, 8> kPadBit{4, 5, 6, 7, 1, 0, 8, 3};
constexpr std::size_t kMaxFrames = 65536;

using Bytes = std::vector<std::uint8_t>;
using View = std::span<const std::uint8_t>;

void put(Bytes &b, std::uint64_t v, unsigned n) {
  for (unsigned i = 0; i < n; ++i)
    b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void field(Bytes &b, std::uint32_t tag, std::uint32_t type, const Bytes &v) {
  put(b, tag, 4);
  put(b, type, 4);
  put(b, v.size(), 8);
  b.insert(b.end(), v.begin(), v.end());
}
void scalar(Bytes &b, std::uint32_t tag, std::uint32_t type, std::uint64_t n) {
  Bytes v;
  put(v, n, type <= 2 ? 4 : 8);
  field(b, tag, type, v);
}
Bytes array(const std::vector<Bytes> &items) {
  Bytes b;
  put(b, items.size(), 8);
  for (const auto &item : items) {
    put(b, item.size(), 8);
    b.insert(b.end(), item.begin(), item.end());
  }
  return b;
}

// Reads n little-endian bytes at `at`; sets ok=false when out of range.
std::uint64_t read(View b, std::size_t at, unsigned n, bool &ok) {
  if (at > b.size() || n > b.size() - at) {
    ok = false;
    return 0;
  }
  std::uint64_t v = 0;
  for (unsigned i = 0; i < n; ++i)
    v |= std::uint64_t(b[at + i]) << (8 * i);
  return v;
}

struct Field {
  std::uint32_t tag = 0, type = 0;
  View data;
};
std::optional<std::vector<Field>> record(View b) {
  std::vector<Field> out;
  std::uint32_t previous = 0;
  while (!b.empty()) {
    if (b.size() < 16)
      return std::nullopt;
    bool ok = true;
    const auto tag = static_cast<std::uint32_t>(read(b, 0, 4, ok));
    const auto type = static_cast<std::uint32_t>(read(b, 4, 4, ok));
    const auto n = read(b, 8, 8, ok);
    if (tag <= previous || type < 1 || type > 11 || n > b.size() - 16)
      return std::nullopt;
    out.push_back({tag, type, b.subspan(16, static_cast<std::size_t>(n))});
    previous = tag;
    b = b.subspan(16 + static_cast<std::size_t>(n));
  }
  return out;
}
std::optional<std::vector<View>> elements(View b, std::uint64_t maximum) {
  bool ok = true;
  const auto n = read(b, 0, 8, ok);
  if (!ok || n > maximum)
    return std::nullopt;
  b = b.subspan(8);
  std::vector<View> out;
  for (std::uint64_t i = 0; i < n; ++i) {
    const auto len = read(b, 0, 8, ok);
    if (!ok || len > b.size() - 8)
      return std::nullopt;
    out.push_back(b.subspan(8, static_cast<std::size_t>(len)));
    b = b.subspan(8 + static_cast<std::size_t>(len));
  }
  if (!b.empty())
    return std::nullopt;
  return out;
}
bool expect(const std::vector<Field> &f, std::size_t i, std::uint32_t tag,
            std::uint32_t type, std::size_t bytes) {
  return i < f.size() && f[i].tag == tag && f[i].type == type &&
         f[i].data.size() == bytes;
}

std::string to_hex(View v) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string s;
  for (const auto byte : v) {
    s.push_back(digits[byte >> 4]);
    s.push_back(digits[byte & 15]);
  }
  return s;
}
bool from_hex(const std::string &s, std::array<std::uint8_t, 32> &out) {
  if (s.size() != 64)
    return false;
  const auto digit = [](char c) -> int {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return -1;
  };
  for (std::size_t i = 0; i < 32; ++i) {
    const int hi = digit(s[2 * i]), lo = digit(s[2 * i + 1]);
    if (hi < 0 || lo < 0)
      return false;
    out[i] = static_cast<std::uint8_t>(hi * 16 + lo);
  }
  return true;
}

} // namespace

std::array<std::int32_t, 8> controls_from_pad(std::uint16_t pad) {
  std::array<std::int32_t, 8> c{};
  for (std::size_t k = 0; k < 8; ++k)
    c[k] = (pad >> kPadBit[k]) & 1U;
  return c;
}

std::uint16_t pad_from_controls(const std::array<std::int32_t, 8> &controls) {
  unsigned pad = 0;
  for (std::size_t k = 0; k < 8; ++k)
    if (controls[k] != 0)
      pad |= 1U << kPadBit[k];
  return static_cast<std::uint16_t>(pad);
}

Bytes encode_trace(const std::vector<std::uint16_t> &pads,
                   const std::array<std::uint8_t, 32> &content,
                   const std::array<std::uint8_t, 32> &config) {
  Bytes out{'T', 'S', 'T', 'R', 'A', 'C', 'E', '1'};
  field(out, 1, 5, Bytes(content.begin(), content.end()));
  field(out, 2, 5, Bytes(config.begin(), config.end()));
  scalar(out, 3, 3, pads.size());
  std::vector<Bytes> frames;
  frames.reserve(pads.size());
  std::uint64_t next = 1;
  for (const auto pad : pads) {
    const auto controls = controls_from_pad(pad);
    std::vector<Bytes> items;
    for (std::size_t k = 0; k < 8; ++k) {
      Bytes item;
      scalar(item, 1, 1, 0);
      scalar(item, 2, 1, k);
      scalar(item, 3, 2, static_cast<std::uint32_t>(controls[k]));
      items.push_back(std::move(item));
    }
    Bytes frame;
    scalar(frame, 1, 3, next++);
    field(frame, 2, 8, array(items));
    frames.push_back(std::move(frame));
  }
  field(out, 4, 8, array(frames));
  return out;
}

std::optional<Trace> decode_trace(View bytes) {
  static constexpr char magic[] = "TSTRACE1";
  if (bytes.size() < 8 || std::memcmp(bytes.data(), magic, 8) != 0)
    return std::nullopt;
  const auto f = record(bytes.subspan(8));
  if (!f || f->size() != 4 || !expect(*f, 0, 1, 5, 32) ||
      !expect(*f, 1, 2, 5, 32) || !expect(*f, 2, 3, 3, 8) ||
      (*f)[3].tag != 4 || (*f)[3].type != 8)
    return std::nullopt;
  bool ok = true;
  const auto n = read((*f)[2].data, 0, 8, ok);
  if (n == 0 || n > kMaxFrames)
    return std::nullopt;
  const auto frames = elements((*f)[3].data, kMaxFrames);
  if (!frames || frames->size() != n)
    return std::nullopt;
  Trace t;
  t.content_sha_hex = to_hex((*f)[0].data);
  t.config_sha_hex = to_hex((*f)[1].data);
  for (std::size_t i = 0; i < frames->size(); ++i) {
    const auto r = record((*frames)[i]);
    if (!r || r->size() != 2 || !expect(*r, 0, 1, 3, 8) || (*r)[1].tag != 2 ||
        (*r)[1].type != 8)
      return std::nullopt;
    if (read((*r)[0].data, 0, 8, ok) != i + 1)
      return std::nullopt;
    const auto items = elements((*r)[1].data, 64);
    if (!items || items->size() != 8)
      return std::nullopt;
    std::array<std::int32_t, 8> controls{};
    for (std::size_t k = 0; k < 8; ++k) {
      const auto e = record((*items)[k]);
      if (!e || e->size() != 3 || !expect(*e, 0, 1, 1, 4) ||
          !expect(*e, 1, 2, 1, 4) || !expect(*e, 2, 3, 2, 4))
        return std::nullopt;
      const auto value = read((*e)[2].data, 0, 4, ok);
      if (read((*e)[0].data, 0, 4, ok) != 0 ||
          read((*e)[1].data, 0, 4, ok) != k || value > 1)
        return std::nullopt;
      controls[k] = static_cast<std::int32_t>(value);
    }
    t.pads.push_back(pad_from_controls(controls));
  }
  return t;
}

int trace_from_arp_command(const std::string &arp_path, const std::string &out_path,
                           const std::string &content_sha_hex,
                           const std::string &config_sha_hex) {
  std::array<std::uint8_t, 32> content{}, config{};
  if (!from_hex(content_sha_hex, content) || !from_hex(config_sha_hex, config))
    return 2;
  const auto take = AytherRecording::load(arp_path);
  if (!take || take->inputs.empty() || take->inputs.size() > kMaxFrames)
    return 1;
  const auto bytes = encode_trace(take->inputs, content, config);
  std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
  if (!out)
    return 4;
  out.write(reinterpret_cast<const char *>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  out.close();
  return out.good() ? 0 : 4;
}

} // namespace ayther::parity
