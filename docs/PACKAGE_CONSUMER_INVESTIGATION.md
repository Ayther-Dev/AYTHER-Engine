# Installed package consumer: hang triage

**Status:** closed investigation (2026-08-31); this page keeps only the
conclusions that still apply.

## Conclusions

- A link-only consumer of the installed package must terminate. CI enforces
  this with `tools/run_package_consumer.ps1`, which fails the job when the
  no-argument consumer does not exit within 5 seconds.
- The link-only mode (`tests/package_consumer/main.cpp` with no core or ROM)
  runs version, capability, and typed-value checks such as `version()`,
  `probe_capabilities()`, `core_abi_revision()`, and `sdk_version_check()`. It
  does not create `AytherSession`, load a core or ROM, initialize SDL, audio,
  or Vulkan, or start Engine threads.
- A Debug consumer cannot link a Release-mode Engine library on MSVC
  (`_ITERATOR_DEBUG_LEVEL` mismatch). That is an expected build-mode
  incompatibility, not a hang; see
  [API and compatibility](API_COMPATIBILITY.md#removal-deprecation-and-security-fixes).

## Reporting a new hang

Record the exact executable, build mode, installed package prefix, arguments,
and the last line the consumer printed. Supplying a core or ROM selects run
mode, which moves the investigation into session and emulator creation,
renderer, and audio startup.
