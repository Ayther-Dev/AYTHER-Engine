#include "session/visual_state_codec.h"

#include <bit>
#include <cmath>
#include <type_traits>
#include <utility>

namespace ayther::session::visual {
namespace {

class Writer {
public:
  template <typename T> void put(T value) {
    static_assert(std::is_integral_v<T>);
    using U = std::make_unsigned_t<T>;
    const auto bits = static_cast<U>(value);
    for (std::size_t i = 0; i < sizeof(T); ++i)
      out_.push_back(static_cast<std::byte>((bits >> (8U * i)) & 0xFFU));
  }
  void put_bool(bool value) { put<std::uint8_t>(value ? 1 : 0); }
  void put_double(double value) { put(std::bit_cast<std::uint64_t>(value)); }
  void put_float(float value) { put(std::bit_cast<std::uint32_t>(value)); }
  [[nodiscard]] std::vector<std::byte> take() { return std::move(out_); }

private:
  std::vector<std::byte> out_;
};

class Reader {
public:
  explicit Reader(std::span<const std::byte> in) : in_(in) {}

  template <typename T> [[nodiscard]] bool get(T &value) {
    static_assert(std::is_integral_v<T>);
    if (in_.size() - pos_ < sizeof(T))
      return false;
    using U = std::make_unsigned_t<T>;
    U bits = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i)
      bits = static_cast<U>(
          bits |
          static_cast<U>(
              static_cast<U>(std::to_integer<std::uint8_t>(in_[pos_ + i]))
              << (8U * i)));
    pos_ += sizeof(T);
    value = static_cast<T>(bits);
    return true;
  }
  [[nodiscard]] bool get_bool(bool &value) {
    std::uint8_t byte = 0;
    if (!get(byte) || byte > 1)
      return false;
    value = byte != 0;
    return true;
  }
  // Finite and non-negative: luminance peaks and gains.
  [[nodiscard]] bool get_magnitude(double &value) {
    std::uint64_t bits = 0;
    if (!get(bits))
      return false;
    value = std::bit_cast<double>(bits);
    return std::isfinite(value) && value >= 0.0;
  }
  [[nodiscard]] bool get_magnitude(float &value) {
    std::uint32_t bits = 0;
    if (!get(bits))
      return false;
    value = std::bit_cast<float>(bits);
    return std::isfinite(value) && value >= 0.0F;
  }
  [[nodiscard]] bool get_count(std::size_t &count) {
    std::uint32_t n = 0;
    if (!get(n) || n > kListLimit)
      return false;
    count = n;
    return true;
  }
  [[nodiscard]] bool done() const { return pos_ == in_.size(); }
  [[nodiscard]] std::size_t remaining() const { return in_.size() - pos_; }
  [[nodiscard]] std::span<const std::byte> take(std::size_t n) {
    const auto out = in_.subspan(pos_, n);
    pos_ += n;
    return out;
  }

private:
  std::span<const std::byte> in_;
  std::size_t pos_ = 0;
};

} // namespace

std::size_t section_index(vs::VisualStateSection section) {
  return static_cast<std::size_t>(
      std::countr_zero(vs::visual_state_section(section)));
}

void append_section(std::vector<std::byte> &payload,
                    vs::VisualStateSection section,
                    std::span<const std::byte> body) {
  Writer w;
  w.put(vs::visual_state_section(section));
  w.put(static_cast<std::uint32_t>(body.size()));
  const std::vector<std::byte> head = w.take();
  payload.insert(payload.end(), head.begin(), head.end());
  payload.insert(payload.end(), body.begin(), body.end());
}

bool split_sections(std::span<const std::byte> payload, std::uint32_t sections,
                    SectionBodies &out) {
  out = {};
  Reader r(payload);
  std::uint32_t seen = 0;
  std::uint32_t previous = 0;
  while (!r.done()) {
    std::uint32_t bit = 0;
    std::uint32_t size = 0;
    if (!r.get(bit) || !r.get(size))
      return false;
    if (!std::has_single_bit(bit) || (bit & sections) == 0 || bit <= previous ||
        size > r.remaining())
      return false;
    out[static_cast<std::size_t>(std::countr_zero(bit))] = r.take(size);
    seen |= bit;
    previous = bit;
  }
  return seen == sections;
}

std::vector<std::byte> encode(const ScreenRecognition &value) {
  Writer w;
  w.put(value.active);
  w.put(value.candidate);
  w.put(value.streak);
  return w.take();
}

bool decode(std::span<const std::byte> body, ScreenRecognition &out) {
  Reader r(body);
  ScreenRecognition v;
  if (!r.get(v.active) || !r.get(v.candidate) || !r.get(v.streak) ||
      v.streak < 0 || !r.done())
    return false;
  out = v;
  return true;
}

std::vector<std::byte> encode(const LevelCamera &value) {
  Writer w;
  for (const std::int32_t x : value.cam_x)
    w.put(x);
  for (const std::int32_t y : value.cam_y)
    w.put(y);
  for (const std::int16_t h : value.prev_h)
    w.put(h);
  for (const std::int16_t v : value.prev_v)
    w.put(v);
  w.put(value.last_frame);
  w.put_bool(value.valid);
  w.put(value.pano_last_id);
  w.put(value.pano_last_x);
  w.put(value.pano_last_y);
  return w.take();
}

bool decode(std::span<const std::byte> body, LevelCamera &out) {
  Reader r(body);
  LevelCamera v;
  for (std::int32_t &x : v.cam_x)
    if (!r.get(x))
      return false;
  for (std::int32_t &y : v.cam_y)
    if (!r.get(y))
      return false;
  for (std::int16_t &h : v.prev_h)
    if (!r.get(h))
      return false;
  for (std::int16_t &pv : v.prev_v)
    if (!r.get(pv))
      return false;
  if (!r.get(v.last_frame) || !r.get_bool(v.valid) || !r.get(v.pano_last_id) ||
      !r.get(v.pano_last_x) || !r.get(v.pano_last_y) || !r.done())
    return false;
  out = v;
  return true;
}

std::vector<std::byte> encode(const PaletteLuma &value) {
  Writer w;
  for (const double peak : value.peak)
    w.put_double(peak);
  return w.take();
}

bool decode(std::span<const std::byte> body, PaletteLuma &out) {
  Reader r(body);
  PaletteLuma v;
  for (double &peak : v.peak)
    if (!r.get_magnitude(peak))
      return false;
  if (!r.done())
    return false;
  out = v;
  return true;
}

std::vector<std::byte> encode(const PreviousAudioMask &value) {
  Writer w;
  w.put(value.mask);
  return w.take();
}

bool decode(std::span<const std::byte> body, PreviousAudioMask &out) {
  Reader r(body);
  PreviousAudioMask v;
  if (!r.get(v.mask) || !r.done())
    return false;
  out = v;
  return true;
}

std::vector<std::byte> encode(std::span<const PlaneSequenceClock> clocks) {
  Writer w;
  w.put(static_cast<std::uint32_t>(clocks.size()));
  for (const PlaneSequenceClock &c : clocks) {
    w.put(c.id);
    w.put(c.anchor);
    w.put(c.last_seen);
  }
  return w.take();
}

bool decode(std::span<const std::byte> body,
            std::vector<PlaneSequenceClock> &out) {
  Reader r(body);
  std::size_t n = 0;
  if (!r.get_count(n))
    return false;
  std::vector<PlaneSequenceClock> v(n);
  for (std::size_t i = 0; i < n; ++i) {
    if (!r.get(v[i].id) || !r.get(v[i].anchor) || !r.get(v[i].last_seen))
      return false;
    if (i > 0 && v[i].id <= v[i - 1].id)
      return false;
  }
  if (!r.done())
    return false;
  out = std::move(v);
  return true;
}

std::vector<std::byte> encode(const Cinematic &value) {
  Writer w;
  w.put(value.active);
  w.put(value.step);
  w.put(value.gap);
  w.put(value.last_frame);
  w.put(value.video_kinematic);
  w.put(value.video_step);
  w.put(value.video_anchor);
  w.put(value.audio_kinematic);
  w.put(value.audio_anchor);
  w.put_bool(value.audio_on);
  w.put(value.audio_last_frame);
  w.put(value.audio_still);
  w.put_float(value.audio_gain);
  return w.take();
}

bool decode(std::span<const std::byte> body, Cinematic &out) {
  Reader r(body);
  Cinematic v;
  if (!r.get(v.active) || !r.get(v.step) || !r.get(v.gap) ||
      !r.get(v.last_frame) || !r.get(v.video_kinematic) ||
      !r.get(v.video_step) || !r.get(v.video_anchor) ||
      !r.get(v.audio_kinematic) || !r.get(v.audio_anchor) ||
      !r.get_bool(v.audio_on) || !r.get(v.audio_last_frame) ||
      !r.get(v.audio_still) || !r.get_magnitude(v.audio_gain) || !r.done())
    return false;
  out = v;
  return true;
}

std::vector<std::byte> encode(std::span<const HdAnimationPhase> phases) {
  Writer w;
  w.put(static_cast<std::uint32_t>(phases.size()));
  for (const HdAnimationPhase &p : phases) {
    w.put(p.clip_id);
    w.put(p.last_pose);
    w.put(p.pose_start_frame);
  }
  return w.take();
}

bool decode(std::span<const std::byte> body,
            std::vector<HdAnimationPhase> &out) {
  Reader r(body);
  std::size_t n = 0;
  if (!r.get_count(n))
    return false;
  std::vector<HdAnimationPhase> v(n);
  for (std::size_t i = 0; i < n; ++i) {
    if (!r.get(v[i].clip_id) || !r.get(v[i].last_pose) ||
        !r.get(v[i].pose_start_frame) || v[i].last_pose < -1)
      return false;
    if (i > 0 && v[i].clip_id <= v[i - 1].clip_id)
      return false;
  }
  if (!r.done())
    return false;
  out = std::move(v);
  return true;
}

std::vector<std::byte> encode(std::span<const PanoramaTint> tints) {
  Writer w;
  w.put(static_cast<std::uint32_t>(tints.size()));
  for (const PanoramaTint &t : tints) {
    w.put(t.id);
    w.put_double(t.ref_luma);
    for (const double v : t.ref_w)
      w.put_double(v);
    for (const double v : t.ref_ch)
      w.put_double(v);
    w.put_bool(t.ref_chroma);
    w.put_double(t.ref_peak);
  }
  return w.take();
}

bool decode(std::span<const std::byte> body, std::vector<PanoramaTint> &out) {
  Reader r(body);
  std::size_t n = 0;
  if (!r.get_count(n))
    return false;
  std::vector<PanoramaTint> v(n);
  for (std::size_t i = 0; i < n; ++i) {
    PanoramaTint &t = v[i];
    if (!r.get(t.id) || !r.get_magnitude(t.ref_luma))
      return false;
    for (double &w : t.ref_w)
      if (!r.get_magnitude(w))
        return false;
    for (double &ch : t.ref_ch)
      if (!r.get_magnitude(ch))
        return false;
    if (!r.get_bool(t.ref_chroma) || !r.get_magnitude(t.ref_peak))
      return false;
    if (i > 0 && t.id <= v[i - 1].id)
      return false;
  }
  if (!r.done())
    return false;
  out = std::move(v);
  return true;
}

} // namespace ayther::session::visual
