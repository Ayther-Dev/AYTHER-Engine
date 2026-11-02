#include "tessera_fx01_oracle.hpp"
#include <algorithm>
#include <array>
#include <ayther/ayther_core_ffi.h>
#include <stdexcept>
#include <string_view>

namespace ayther::test::tessera_fx01 {
namespace {
void require(bool condition, const char *code) {
  if (!condition)
    throw std::runtime_error(code);
}
Bytes text(std::string_view value) { return {value.begin(), value.end()}; }
void put(Bytes &out, std::uint64_t value, unsigned count) {
  for (unsigned byte = 0; byte < count; ++byte)
    out.push_back(static_cast<std::uint8_t>(value >> (byte * 8)));
}
void field(Bytes &out, std::uint32_t tag, std::uint32_t encoding, View value) {
  put(out, tag, 4);
  put(out, encoding, 4);
  put(out, value.size(), 8);
  out.insert(out.end(), value.begin(), value.end());
}
void scalar(Bytes &out, std::uint32_t tag, std::uint32_t encoding,
            std::uint64_t value) {
  Bytes data;
  put(data, value, encoding <= 2 ? 4 : 8);
  field(out, tag, encoding, data);
}
Bytes array(const std::vector<Bytes> &values) {
  Bytes out;
  put(out, values.size(), 8);
  for (const auto &value : values) {
    put(out, value.size(), 8);
    out.insert(out.end(), value.begin(), value.end());
  }
  return out;
}
Bytes sha(View value) {
  const auto encoded = digest(value);
  Bytes out;
  for (std::size_t index = 0; index < encoded.size(); index += 2) {
    const auto digit = [](char ch) {
      return ch <= '9' ? ch - '0' : ch - 'a' + 10;
    };
    out.push_back(static_cast<std::uint8_t>(digit(encoded[index]) * 16 +
                                            digit(encoded[index + 1])));
  }
  return out;
}
Bytes input_wire(unsigned mask) {
  std::vector<Bytes> assignments;
  for (unsigned control = 0; control < 8; ++control) {
    Bytes item;
    scalar(item, 1, 1, 0);
    scalar(item, 2, 1, control);
    scalar(item, 3, 2, (mask >> control) & 1);
    assignments.push_back(std::move(item));
  }
  return array(assignments);
}
} // namespace

std::string digest(View value) {
  std::array<char, 65> out{};
  require(ayther_sha256_hex(value.data(), value.size(), out.data()),
          "ORACLE_SHA_FAILED");
  return out.data();
}
std::string hex_bytes(View value) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(value.size() * 2);
  for (const auto byte : value) {
    result += digits[byte >> 4];
    result += digits[byte & 15];
  }
  return result;
}
Bytes initial_memory() {
  Bytes value;
  for (unsigned index = 0; index < 16; ++index)
    value.push_back(static_cast<std::uint8_t>(3 + 17 * index));
  return value;
}
Bytes content(bool complete, unsigned flags) {
  Bytes value = text("TSSYN001");
  for (const auto field_value : {complete ? 2U : 1U, 7U, flags, 0U})
    put(value, field_value, 4);
  return value;
}
Bytes configuration(const engine::NativeBackendInfo &backend) {
  Bytes value;
  scalar(value, 1, 1, 0);
  field(value, 2, 6, text(backend.id));
  field(value, 3, 5, backend.build_id);
  scalar(value, 4, 1, 0);
  scalar(value, 5, 1, 48000);
  std::vector<Bytes> controls;
  for (unsigned control = 0; control < 8; ++control) {
    Bytes item;
    scalar(item, 1, 1, 0);
    scalar(item, 2, 1, control);
    scalar(item, 3, 1, 1);
    scalar(item, 4, 2, 0);
    scalar(item, 5, 2, 1);
    controls.push_back(std::move(item));
  }
  field(value, 6, 8, array(controls));
  field(value, 7, 5, sha(initial_memory()));
  return value;
}
Bytes neutral_trace(const engine::NativeConfig &configuration,
                    std::uint64_t frames) {
  auto out = text("TSTRACE1");
  field(out, 1, 5, sha(configuration.content));
  field(out, 2, 5, sha(configuration.config));
  scalar(out, 3, 3, frames);
  std::vector<Bytes> records;
  for (std::uint64_t frame = 1; frame <= frames; ++frame) {
    Bytes record;
    scalar(record, 1, 3, frame);
    field(record, 2, 8, input_wire(0));
    records.push_back(std::move(record));
  }
  field(out, 4, 8, array(records));
  return out;
}
Expected expected(const engine::NativeBackendInfo &backend, bool action,
                  std::uint64_t frame) {
  const bool complete = backend.id == "synthetic.complete";
  require(complete || backend.id == "synthetic.minimal",
          "ORACLE_BACKEND_UNKNOWN");
  const auto count =
      action && frame > 300 ? std::min<std::uint64_t>(frame - 300, 300) : 0;
  Expected out;
  out.mask = action && frame >= 301 && frame <= 600 ? 32 : 0;
  const auto accumulator = static_cast<std::uint32_t>(7 + frame + 32 * count);
  out.memory = initial_memory();
  for (std::size_t index = 0; index < out.memory.size(); ++index)
    out.memory[index] = static_cast<std::uint8_t>(out.memory[index] + frame +
                                                  (index % 8 == 5 ? count : 0));
  out.state = text("TSSTATE1");
  field(out.state, 1, 6, text("tessera.synthetic"));
  scalar(out.state, 2, 1, 1);
  scalar(out.state, 3, 1, 0);
  field(out.state, 4, 5, sha(content(complete)));
  field(out.state, 5, 5, backend.build_id);
  field(out.state, 6, 5, sha(configuration(backend)));
  scalar(out.state, 7, 3, frame);
  scalar(out.state, 8, 3, 1000 * frame);
  field(out.state, 9, 8, input_wire(out.mask));
  Bytes machine;
  scalar(machine, 1, 1, accumulator);
  field(machine, 2, 5, out.memory);
  scalar(machine, 3, 1, (7 + 800 * frame) % 256);
  field(out.state, 65536, 11, machine);
  for (unsigned y = 0; y < 16; ++y)
    for (unsigned x = 0; x < 16; ++x) {
      const bool sprite = x == (out.mask & 7) && y == ((out.mask >> 3) & 7);
      const auto px = sprite ? 15 : x, py = sprite ? 15 : y;
      out.pixels.push_back(static_cast<std::uint8_t>(px + out.mask));
      out.pixels.push_back(static_cast<std::uint8_t>(py + accumulator));
      out.pixels.push_back(
          static_cast<std::uint8_t>(out.memory[(px + py) % 16] + frame));
      out.pixels.push_back(255);
    }
  const auto previous = (7 + 800 * (frame ? frame - 1 : 0)) % 256;
  const auto amplitude = 1000 + 4 * static_cast<std::int32_t>(out.mask);
  for (unsigned sample = 0; sample < 800; ++sample) {
    const auto left = (previous + sample) % 256 < 128 ? amplitude : -amplitude;
    put(out.pcm, static_cast<std::uint16_t>(left), 2);
    put(out.pcm, static_cast<std::uint16_t>(-left), 2);
  }
  return out;
}
Digests canonical(View state, const engine::NativeFrame &frame) {
  require(frame.width == 16 && frame.height == 16 && frame.row_bytes == 64 &&
              frame.video_format == 1 && frame.pixels.size() == 1024,
          "ORIGINAL_VIDEO_FORMAT_INVALID");
  require(frame.sample_rate == 48000 && frame.channels == 2 &&
              frame.pcm_frames == 800 && frame.pcm.size() == 3200,
          "ORIGINAL_PCM_FORMAT_INVALID");
  auto video = text("TSVIDEO1");
  for (const auto value : {frame.width, frame.height, frame.video_format})
    put(video, value, 4);
  video.insert(video.end(), frame.pixels.begin(), frame.pixels.end());
  auto audio = text("TSAUDIO1");
  put(audio, frame.sample_rate, 4);
  put(audio, frame.channels, 4);
  put(audio, frame.pcm_frames, 8);
  audio.insert(audio.end(), frame.pcm.begin(), frame.pcm.end());
  return {digest(state), digest(video), digest(audio)};
}
void verify(const engine::NativeBackendInfo &backend, bool action,
            std::uint64_t frame, View state,
            const engine::NativeFrame &original) {
  const auto model = expected(backend, action, frame);
  require(std::ranges::equal(state, model.state),
          "INDEPENDENT_STATE_ORACLE_DIVERGENCE");
  require(std::ranges::equal(original.pixels, model.pixels),
          "INDEPENDENT_VIDEO_ORACLE_DIVERGENCE");
  require(std::ranges::equal(original.pcm, model.pcm),
          "INDEPENDENT_AUDIO_ORACLE_DIVERGENCE");
  require(original.frame_id.frame == frame &&
              original.tick_begin == (frame - 1) * 1000 &&
              original.tick_end == frame * 1000,
          "ORIGINAL_FRAME_CLOCK_DIVERGENCE");
}
} // namespace ayther::test::tessera_fx01
