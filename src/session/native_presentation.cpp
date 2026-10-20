#include "session/native_presentation.h"

#include "ayther_core_ffi.h"
#include "decode_limits.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <stb_image.h>

namespace ayther::session {
namespace {
using Bytes = std::span<const std::uint8_t>;

std::uint64_t read_le(Bytes data, std::size_t at, unsigned count) {
  std::uint64_t value = 0;
  for (unsigned i = 0; i < count; ++i)
    value |= std::uint64_t{data[at + i]} << (8 * i);
  return value;
}

struct Field {
  std::uint32_t tag{}, encoding{};
  Bytes bytes;
};

bool parse_fields(Bytes bytes, std::array<Field, 8> &fields,
                  std::size_t &count) {
  count = 0;
  std::size_t at = 0;
  std::uint32_t previous = 0;
  while (at < bytes.size()) {
    if (bytes.size() - at < 16 || count == fields.size())
      return false;
    const auto tag = static_cast<std::uint32_t>(read_le(bytes, at, 4));
    const auto encoding = static_cast<std::uint32_t>(read_le(bytes, at + 4, 4));
    const auto length = read_le(bytes, at + 8, 8);
    at += 16;
    if (tag <= previous || tag > 8 || length > bytes.size() - at)
      return false;
    fields[count++] = {tag, encoding,
                       bytes.subspan(at, static_cast<std::size_t>(length))};
    at += static_cast<std::size_t>(length);
    previous = tag;
  }
  return true;
}

Bytes field(const std::array<Field, 8> &fields, std::size_t count,
            std::uint32_t tag, std::uint32_t encoding) {
  for (std::size_t i = 0; i < count; ++i)
    if (fields[i].tag == tag && fields[i].encoding == encoding)
      return fields[i].bytes;
  return {};
}

struct Cell {
  std::uint16_t x{}, y{}, word{};
  std::uint64_t visible{};
  std::array<std::uint8_t, 256> rgba{};
};

bool load_asset(const AyArchive *pack, const std::string &name,
                std::array<std::uint8_t, 256> &rgba) {
  std::string path = name;
  auto size = ayther_pack_file_size(pack, path.c_str());
  if (size <= 0) {
    path = "assets/" + name;
    size = ayther_pack_file_size(pack, path.c_str());
  }
  if (size <= 0 || size > 4 * 1024 * 1024)
    return false;
  std::vector<std::uint8_t> encoded(static_cast<std::size_t>(size));
  if (ayther_pack_read(pack, path.c_str(), encoded.data(), encoded.size()) !=
      size)
    return false;
  int width = 0, height = 0, channels = 0;
  if (!limits::image_header_within_limits(encoded.data(), encoded.size(),
                                          &width, &height) ||
      width != 8 || height != 8 ||
      encoded.size() >
          static_cast<std::size_t>(std::numeric_limits<int>::max()))
    return false;
  auto *decoded =
      stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()),
                            &width, &height, &channels, 4);
  if (!decoded || width != 8 || height != 8) {
    stbi_image_free(decoded);
    return false;
  }
  std::memcpy(rgba.data(), decoded, rgba.size());
  stbi_image_free(decoded);
  return true;
}

} // namespace

NativeComposeResult compose_native_frame(
    const engine::NativeFrame &original, const engine::NativeRegion &temporal,
    const AyArchive *pack,
    const std::unordered_map<std::uint64_t, std::string> &overrides,
    std::vector<std::uint8_t> &presented) {
  presented.clear();
  if (!pack || original.video_format != engine::native_video_rgba8 ||
      original.width != 320 || original.height != 224 ||
      original.row_bytes != original.width * 4 ||
      original.pixels.size() !=
          std::size_t(original.row_bytes) * original.height)
    return NativeComposeResult::unsupported;
  if (temporal.kind != 5 || temporal.encoding != 5 ||
      temporal.byte_count != temporal.data.size() || temporal.flags != 3)
    return NativeComposeResult::incomplete;
  std::array<Field, 8> fields{};
  std::size_t field_count = 0;
  if (!parse_fields(temporal.data, fields, field_count) || field_count != 8)
    return NativeComposeResult::invalid_data;
  for (std::uint32_t tag : {1U, 2U, 3U, 4U, 6U}) {
    const auto value = field(fields, field_count, tag, 1);
    if (value.size() != 4)
      return NativeComposeResult::invalid_data;
  }
  if (read_le(field(fields, field_count, 1, 1), 0, 4) != 2 ||
      read_le(field(fields, field_count, 2, 1), 0, 4) != original.width ||
      read_le(field(fields, field_count, 3, 1), 0, 4) != original.height ||
      read_le(field(fields, field_count, 4, 1), 0, 4) != 1 ||
      read_le(field(fields, field_count, 6, 1), 0, 4) != 1)
    return NativeComposeResult::incomplete;
  const auto cells_data = field(fields, field_count, 7, 5);
  const auto sources = field(fields, field_count, 8, 5);
  if (cells_data.size() < 4 || sources.size() != 320 * 224 * 8)
    return NativeComposeResult::invalid_data;
  const auto count = read_le(cells_data, 0, 4);
  if (count > 40 * 28 || cells_data.size() != 4 + count * 272)
    return NativeComposeResult::invalid_data;
  std::vector<Cell> cells;
  cells.reserve(static_cast<std::size_t>(count));
  std::array<bool, 40 * 28> seen{};
  for (std::size_t index = 0; index < count; ++index) {
    const auto at = 4 + index * 272;
    Cell cell;
    cell.x = static_cast<std::uint16_t>(read_le(cells_data, at, 2));
    cell.y = static_cast<std::uint16_t>(read_le(cells_data, at + 2, 2));
    cell.word = static_cast<std::uint16_t>(read_le(cells_data, at + 4, 2));
    if (read_le(cells_data, at + 6, 2) != 0 || cell.x >= 320 || cell.y >= 224 ||
        cell.x % 8 || cell.y % 8 || (cell.word & 0x1800) != 0)
      return NativeComposeResult::invalid_data;
    cell.visible = read_le(cells_data, at + 8, 8);
    if (cell.visible == 0)
      return NativeComposeResult::invalid_data;
    const auto slot = std::size_t(cell.y / 8) * 40 + cell.x / 8;
    if (seen[slot])
      return NativeComposeResult::invalid_data;
    seen[slot] = true;
    std::copy_n(cells_data.begin() + at + 16, 256, cell.rgba.begin());
    for (unsigned bit = 0; bit < 64; ++bit) {
      if ((cell.visible & (std::uint64_t{1} << bit)) == 0)
        continue;
      const auto x = cell.x + bit % 8, y = cell.y + bit / 8;
      const auto source_at = (std::size_t(y) * 320 + x) * 8;
      if (sources[source_at + 4] != 1 ||
          read_le(sources, source_at + 2, 2) != cell.word ||
          sources[source_at + 6] != bit % 8 ||
          sources[source_at + 7] != bit / 8)
        return NativeComposeResult::invalid_data;
    }
    cells.push_back(std::move(cell));
  }

  std::unique_ptr<AytherTileHasher, decltype(&ayther_tile_hasher_free)> hasher(
      ayther_tile_hasher_new(), &ayther_tile_hasher_free);
  std::unique_ptr<AytherTileSubstitutor, decltype(&ayther_tile_sub_free)>
      catalog(ayther_tile_sub_new(), &ayther_tile_sub_free);
  if (!hasher || !catalog)
    return NativeComposeResult::invalid_data;
  ayther_tile_sub_load_pack(catalog.get(), pack);
  ayther_tile_sub_begin_frame(catalog.get(), original.frame_id.frame, nullptr,
                              0, false);
  for (const auto &[hash, path] : overrides)
    ayther_tile_sub_add_override(catalog.get(), hash, path.c_str());
  std::map<std::array<std::uint8_t, 256>, std::uint64_t> hashes;
  std::map<std::string, std::array<std::uint8_t, 256>> decoded;
  std::vector<std::pair<const Cell *, const std::array<std::uint8_t, 256> *>>
      drawing;
  for (const auto &cell : cells) {
    auto found = hashes.find(cell.rgba);
    if (found == hashes.end()) {
      std::array<std::uint8_t, 256> bgra = cell.rgba;
      for (std::size_t pixel = 0; pixel < 64; ++pixel) {
        std::swap(bgra[pixel * 4], bgra[pixel * 4 + 2]);
        bgra[pixel * 4 + 3] = 255;
      }
      ayther_tile_hasher_process_frame(hasher.get(), bgra.data(), 8, 8, 32, 1);
      AytherTileOccurrence occurrence{};
      if (ayther_tile_hasher_get_occurrences(hasher.get(), &occurrence, 1) != 1)
        return NativeComposeResult::invalid_data;
      found = hashes.emplace(cell.rgba, occurrence.hash).first;
    }
    std::array<char, 512> path{};
    if (!ayther_tile_sub_lookup(catalog.get(), found->second, path.data(),
                                static_cast<std::uint32_t>(path.size())))
      continue;
    if (path.back() != 0)
      return NativeComposeResult::invalid_data;
    const std::string name(path.data());
    auto asset = decoded.find(name);
    if (asset == decoded.end()) {
      std::array<std::uint8_t, 256> image{};
      if (!load_asset(pack, name, image))
        return NativeComposeResult::invalid_asset;
      asset = decoded.emplace(name, image).first;
    }
    drawing.emplace_back(&cell, &asset->second);
  }
  if (drawing.empty())
    return NativeComposeResult::no_matching_rule;
  presented.assign(original.pixels.begin(), original.pixels.end());
  for (const auto &[cell, image] : drawing)
    for (unsigned bit = 0; bit < 64; ++bit) {
      if ((cell->visible & (std::uint64_t{1} << bit)) == 0)
        continue;
      const auto at =
          (std::size_t(cell->y + bit / 8) * 320 + cell->x + bit % 8) * 4;
      const auto src = bit * 4;
      const auto alpha = (*image)[src + 3];
      for (unsigned channel = 0; channel < 3; ++channel)
        presented[at + channel] = static_cast<std::uint8_t>(
            (std::uint32_t((*image)[src + channel]) * alpha +
             std::uint32_t(presented[at + channel]) * (255 - alpha) + 127) /
            255);
      presented[at + 3] = 255;
    }
  return NativeComposeResult::presented;
}

} // namespace ayther::session
