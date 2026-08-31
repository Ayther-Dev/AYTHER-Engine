# Installed package consumer investigation

**Date:** 2026-08-31

**Question:** why did the fully linked local consumer appear not to terminate?

## Result

The block was not reproduced from a clean installed prefix. The
`RelWithDebInfo` Engine package configured and built an out-of-tree consumer,
and its no-argument link-only mode exited with code 0 inside the five-second
deadline. The same deadline is now enforced in every Windows and Linux native
package-consumer CI job by `tools/run_package_consumer.ps1`.

The last completed operations are observable in the report:

```text
AYTHER SDK 0.1.0 / Engine 0.1.0
  mode: link only (no core/ROM given)
```

Reaching those lines proves that process startup, DLL loading, static
initialization, `version()`, `probe_capabilities()`, and `sdk_version_check()`
completed. The link-only branch returns immediately afterward. It does not
construct `AytherSession`, load a core or ROM, initialize SDL/audio or Vulkan,
open the filesystem, or start Engine threads.

## Configuration distinction

| Consumer configuration | Installed Engine | Result |
|---|---|---|
| `RelWithDebInfo` | `RelWithDebInfo` | Configured, linked, ran, and exited 0 under five seconds |
| `Debug` | `RelWithDebInfo` | Rejected at link time; `_ITERATOR_DEBUG_LEVEL` was 2 in the consumer and 0 in `ayther_engine.lib` |

The Debug/Release mismatch is a comprehensible compatibility failure, not a
runtime hang. The package deliberately does not promise a C++ ABI across build
modes. A Debug consumer must use an Engine library built with the same mode.

## Conclusion

The isolated capability probe and the fully linked link-only consumer both
terminate. No Engine regression test beyond the new bounded package-consumer
gate is warranted because no Engine hang was reproduced. A future report of a
hang should preserve the exact executable, build mode, linked package prefix,
arguments, and the last emitted line; supplying a core or ROM selects run mode
and moves the investigation into session/core, renderer, and audio startup.
