// ---------------------------------------------------------------------------
// rewind_buffer.cpp — zstd-compressed savestate ring. See rewind_buffer.h.
// ---------------------------------------------------------------------------
#include "rewind_buffer.h"

#include <zstd.h>
#include <cstdio>

namespace ayther {

namespace { constexpr int kZstdLevel = 1; }   // fastest — per-frame budget

void RewindBuffer::configure(size_t raw_state_size, uint32_t capacity_frames) {
    raw_size_ = raw_state_size;
    capacity_ = capacity_frames;
    clear();
}

void RewindBuffer::set_enabled(bool on) {
    enabled_ = on;
    if (!on) clear();
}

void RewindBuffer::clear() {
    ring_.clear();
    mem_ = 0;
}

void RewindBuffer::push(const std::vector<uint8_t>& raw) {
    if (!enabled_ || raw.empty() || capacity_ == 0) return;

    // Compress into a worst-case-sized scratch, then shrink to the actual size.
    const size_t bound = ZSTD_compressBound(raw.size());
    std::vector<uint8_t> blob(bound);
    const size_t n = ZSTD_compress(blob.data(), bound, raw.data(), raw.size(), kZstdLevel);
    if (ZSTD_isError(n)) {
        std::fprintf(stderr, "[RewindBuffer] compress failed: %s\n", ZSTD_getErrorName(n));
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

bool RewindBuffer::pop(std::vector<uint8_t>& out_raw) {
    // Need at least 2: drop the current state, restore the previous one.
    if (ring_.size() < 2) return false;

    mem_ -= ring_.back().size();
    ring_.pop_back();

    const std::vector<uint8_t>& blob = ring_.back();   // the new head = previous frame
    out_raw.resize(raw_size_);
    const size_t n = ZSTD_decompress(out_raw.data(), out_raw.size(),
                                     blob.data(), blob.size());
    if (ZSTD_isError(n)) {
        std::fprintf(stderr, "[RewindBuffer] decompress failed: %s\n", ZSTD_getErrorName(n));
        return false;
    }
    out_raw.resize(n);
    return true;
}

}  // namespace ayther
