// RewindBuffer (R6): the zstd savestate ring. Disabled it records nothing;
// enabled it keeps at most `capacity` frames, pop() drops the head and
// restores the previous state, and a state larger than the configured size
// (a native checkpoint, spec 008 TP-302.1) comes back whole.
#include "rewind_buffer.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}

std::vector<std::uint8_t> state(std::uint8_t tag, std::size_t size = 256) {
  std::vector<std::uint8_t> s(size);
  for (std::size_t i = 0; i < size; ++i)
    s[i] = static_cast<std::uint8_t>(tag + (i % 7));
  return s;
}
} // namespace

int main() {
  ayther::RewindBuffer rb;
  rb.configure(256, 3);
  std::vector<std::uint8_t> out;

  rb.push(state(1));
  check(!rb.enabled() && rb.frames() == 0, "disabled: push records nothing");

  rb.set_enabled(true);
  rb.push({});
  check(rb.frames() == 0, "an empty state is ignored");
  check(!rb.pop(out), "pop needs two frames");

  for (std::uint8_t tag = 1; tag <= 5; ++tag)
    rb.push(state(tag));
  check(rb.frames() == 3 && rb.memory_bytes() > 0,
        "the ring keeps the capacity, oldest evicted");

  check(rb.pop(out) && out == state(4), "pop restores the previous frame");
  check(rb.pop(out) && out == state(3), "and walks back one more");
  check(!rb.pop(out) && rb.frames() == 1, "the last frame is not popped");

  rb.push(state(9, 1024));
  rb.push(state(10, 1024));
  check(rb.pop(out) && out == state(9, 1024),
        "a state larger than the configured size comes back whole");

  rb.set_enabled(false);
  check(rb.frames() == 0 && rb.memory_bytes() == 0, "disabling clears");

  rb.set_enabled(true);
  rb.configure(256, 0);
  rb.push(state(1));
  check(rb.frames() == 0, "capacity 0 records nothing");

  std::printf("%s (%d failures)\n", failures ? "FAILED" : "OK", failures);
  return failures ? 1 : 0;
}
