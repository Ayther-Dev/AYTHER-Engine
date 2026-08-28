#pragma once
// ---------------------------------------------------------------------------
// rewind_buffer.h — compressed savestate ring buffer for rewind (R6).
//
// Holds a sliding window of emulator savestates so the frontend can step the
// emulation *backwards*. Each ~1 MB savestate is zstd-compressed (level 1,
// ~30–60 KB) on push, so a 10-second window at 60 fps fits in ~18–36 MB
// instead of ~600 MB raw (see ayther-engine.md §6.1).
//
// Model: push() the current state each frame (END of step). rewind_step()
// drops the current state and restores the previous one, so holding rewind
// walks back one frame per display frame. Disabled by default — when off,
// push() is a no-op and there is zero per-frame cost.
//
// Single-threaded; driven from the emulation thread alongside AytherSession.
// ---------------------------------------------------------------------------
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace ayther {

class RewindBuffer {
public:
    /// Configure for a given raw savestate size + capacity in frames.
    /// (capacity ≈ seconds × fps). Clears any existing contents.
    void configure(size_t raw_state_size, uint32_t capacity_frames);

    void set_enabled(bool on);
    bool enabled() const noexcept { return enabled_; }

    /// Drop all captured states (e.g. on reset / load_state).
    void clear();

    /// Compress + store `raw` as the newest state (no-op when disabled).
    /// Evicts the oldest state when at capacity.
    void push(const std::vector<uint8_t>& raw);

    /// Rewind one frame: drop the current (newest) state and decompress the
    /// previous one into `out_raw`. Returns false if fewer than 2 states exist
    /// (nothing to rewind to). The restored state stays at the buffer's head.
    bool pop(std::vector<uint8_t>& out_raw);

    size_t frames()       const noexcept { return ring_.size(); }
    size_t memory_bytes() const noexcept { return mem_; }

private:
    std::deque<std::vector<uint8_t>> ring_;   // compressed blobs, newest at back
    size_t   raw_size_ = 0;
    uint32_t capacity_ = 0;
    bool     enabled_  = false;
    size_t   mem_      = 0;   // sum of compressed blob sizes
};

}  // namespace ayther
