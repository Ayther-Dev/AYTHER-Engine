// ---------------------------------------------------------------------------
// rewind_buffer.cpp — zstd-compressed savestate ring. See rewind_buffer.h.
// ---------------------------------------------------------------------------
#include "rewind_buffer.h"
#include "log.h"

#include <cstdio>
#include <zstd.h>

namespace ayther {

namespace {
constexpr int kZstdLevel = 1;
} // namespace

void RewindBuffer::configure(size_t raw_state_size, uint32_t capacity_frames) {
  raw_size_ = raw_state_size;
  capacity_ = capacity_frames;
  clear();
}

void RewindBuffer::set_enabled(bool on) {
  enabled_ = on;
  if (!on)
    clear();
}

void RewindBuffer::clear() {
  ring_.clear();
  mem_ = 0;
}

void RewindBuffer::push(const std::vector<uint8_t> &raw) {
  if (!enabled_ || raw.empty() || capacity_ == 0)
    return;

  // Compress into a worst-case-sized scratch, then shrink to the actual size.
  const size_t bound = ZSTD_compressBound(raw.size());
  std::vector<uint8_t> blob(bound);
  const size_t n =
      ZSTD_compress(blob.data(), bound, raw.data(), raw.size(), kZstdLevel);
  if (ZSTD_isError(n)) {
    ayther::log::write(ayther::log::Severity::Error, "rewind",
                       "compress_failed", "compress failed: %s",
                       ZSTD_getErrorName(n));
    return;
  }
  blob.resize(n);
  blob.shrink_to_fit();

  mem_ += blob.size();
  ring_.push_back(std::move(blob));

  // Evict oldest beyond capacity.
  while (ring_.size() > capacity_) {
    mem_ -= ring_.front().size();
    ring_.pop_front();
  }
}

bool RewindBuffer::pop(std::vector<uint8_t> &out_raw) {
  // Need at least 2: drop the current state, restore the previous one.
  if (ring_.size() < 2)
    return false;

  mem_ -= ring_.back().size();
  ring_.pop_back();

  const std::vector<uint8_t> &blob =
      ring_.back(); // the new head = previous frame
  // The frame records its own size (ZSTD_compress always writes it). A
  // native checkpoint (spec 008, TP-302.1) is not a fixed size: the state
  // configured at enable time is only a floor.
  const unsigned long long content =
      ZSTD_getFrameContentSize(blob.data(), blob.size());
  out_raw.resize(content != ZSTD_CONTENTSIZE_UNKNOWN &&
                         content != ZSTD_CONTENTSIZE_ERROR &&
                         content > raw_size_
                     ? static_cast<size_t>(content)
                     : raw_size_);
  const size_t n =
      ZSTD_decompress(out_raw.data(), out_raw.size(), blob.data(), blob.size());
  if (ZSTD_isError(n)) {
    ayther::log::write(ayther::log::Severity::Error, "rewind",
                       "decompress_failed", "decompress failed: %s",
                       ZSTD_getErrorName(n));
    return false;
  }
  out_raw.resize(n);
  return true;
}

} // namespace ayther
