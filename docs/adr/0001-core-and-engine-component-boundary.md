# ADR 0001: Keep core policy separate from native orchestration

- **Status:** Accepted
- **Date:** 2026-08-27

## Context

AYTHER needs deterministic, format-heavy logic and also needs low-latency native
integration with emulator, graphics, audio, and operating-system APIs. Exposing
all implementation detail directly to frontends would duplicate ownership and
make compatibility, testing, and security boundaries unclear.

## Decision

Use two cohesive layers within AYTHER Engine:

- Rust `ayther_core` owns identity, substitution policy, pack semantics,
  validation, scripting, patches, and format processing.
- The C++ engine owns session orchestration, emulator hosting, rendering, audio,
  timing, recording, and native-resource lifetimes.

The preferred native boundary is typed CXX declarations. A flat C ABI remains
available for C consumers and selected pointer-heavy paths, but is not the
preferred high-level product API. External frontends should consume the
engine-owned session facade rather than assemble core implementation details
directly.

## Consequences

- Deterministic logic can be tested without graphics hardware.
- Unsafe pointer and lifetime contracts are concentrated at explicit boundaries.
- Core and engine must evolve together whenever shared types change.
- The installed package can expose `Ayther::core` before the full engine exists,
  but must label that surface partial and unstable.
- A future stable ABI requires exhaustive layout, symbol, ownership, and
  out-of-tree consumption tests.

## Rejected alternatives

- Put all behavior in C++: this weakens memory-safety guarantees for parsing,
  validation, and complex format processing.
- Put native multimedia and emulator hosting entirely in Rust: this adds no
  present benefit sufficient to offset integration complexity and existing C++
  ecosystem requirements.
- Expose core modules directly as the product API: this couples consumers to
  implementation detail and prevents a coherent session lifecycle.
