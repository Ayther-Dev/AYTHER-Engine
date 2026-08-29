# Generated dependency graph

**Status:** generated from the current checkout

> **GENERATED — do not edit by hand.** Run pwsh tools/dep_graph.ps1.
> pwsh tools/dep_graph.ps1 -Check verifies that this document still
> matches Cargo.toml and the configured CMake target graph.

## Rust crates

| Crate | Local path dependencies |
|---|---|
| `ayther_core` | — |

## C++ targets

Derived from `cmake --graphviz` over the configured build.
Only Engine repository and installed `Ayther::*` package targets are included.

```
ayther_cxx -> ayther_core
ayther_engine -> ayther_core
ayther_engine -> ayther_ymfm
```
