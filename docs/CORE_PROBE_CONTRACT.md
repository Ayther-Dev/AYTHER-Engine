# Core probe contract (v2)

Contract shared by the Engine, the Runtime, the Lab and Play CE to decide whether
a libretro core on disk can be used with AYTHER. It extends `probe_core`
(`include/ayther/engine/core_probe.hpp`), which only reads the libretro
information entry points, with the AYTHER ABI and the capability check.

The probe never loads content, never starts a session and never initialises SDL
or Vulkan. A core that is valid for AYTHER announces the AYTHER ABI and every
capability in `required_capabilities`; a core without them is rejected, even
for playing without a pack.

## Verdicts

| `verdict` | `reason` | Process exit | Meaning |
|---|---|---|---|
| `ok` | — | 0 | Loads, announces ABI 1.10 and every required capability. |
| `missing_file` | `core.missing` | 2 | The path does not exist. |
| `wrong_architecture` | `core.wrong_architecture` | 2 | The binary is for another architecture than the host process (for example an x86 DLL in an x64 process). |
| `not_a_core` | `core.invalid` | 3 | The file exists but is not a loadable library, or lacks the libretro information entry points. |
| `abi_missing` | `core.abi_missing` | 4 | A libretro core without the AYTHER extension (a stock core). |
| `capabilities_incomplete` | `core.capabilities_incomplete` | 4 | AYTHER ABI present, but `capabilities` does not cover `required_capabilities`. |
| `timeout` | `core.probe_timeout` | 5 | The probe did not answer in time. The parent process sets this verdict and the exit code; the child never reports it. |

The probe runs in a child process so that a core that hangs or crashes at load
time cannot take the caller down. The parent waits at most the **time limit**
(5 seconds by default, configurable by the caller) and kills the child after it.
Exit code 1 is reserved for a usage error of the probe command itself.

## Output

One line on standard output, prefixed by `AYTHER_STATUS `, followed by one JSON
object. `protocol_version` is `2`.

```json
AYTHER_STATUS {"protocol_version":2,"event":"probe","ok":true,"verdict":"ok",
  "api":1,"library_name":"Genesis Plus GX","library_version":"v1.7.4 7109a7c2",
  "architecture":"x64","ayther_abi":"1.10",
  "ayther_build_id":"Genesis Plus GX AYTHER ABI 1.10; core v1.7.4 7109a7c2",
  "capabilities":"0xEFFFF","required_capabilities":"0xEFFFF","missing_capabilities":[]}

AYTHER_STATUS {"protocol_version":2,"event":"probe","ok":false,"verdict":"capabilities_incomplete",
  "reason":"core.capabilities_incomplete","capabilities":"0x6FFFF","required_capabilities":"0xEFFFF",
  "missing_capabilities":["SPRITE_OUTCOME_V1"],"message":"..."}
```

Fields:

- `ok`, `verdict`: always present. `reason` appears only when `ok` is false.
- `api`, `library_name`, `library_version`, `architecture`: from the libretro
  information entry points and the host, present once the library loaded.
- `ayther_abi`, `ayther_build_id`: from the AYTHER extension, present when it exists.
- `capabilities`: the bit mask the core announces (`AYTHER_CAP_*` of
  `ayther_api.h`, hexadecimal string). `required_capabilities`: the mask the
  Engine requires. `missing_capabilities`: the names of the required bits that
  are not announced, in bit order (without the `AYTHER_CAP_` prefix).
- `message`: free text for logs; never parsed.

## Required capabilities

`required_capabilities` is the constant of the Engine, `0xEFFFF`: bits 0 to 15
and 17 to 19 of `ayther_api.h` (`LEGACY_MEMORY` to `SYSTEM_V1`, then
`LINE_STATE_V1`, `OBSERVABILITY_V1` and `SPRITE_OUTCOME_V1`). Bit 16,
`MODE4_CONTROLS` (render controls in Mode 4), is **not** required: the gpgx r4
fork with the AYTHER observation (ABI 1.10, build id `Genesis Plus GX AYTHER ABI
1.10; core v1.7.4 7109a7c2`) announces `0xEFFFF`, without it, and it is the only
external core supported. The measurement is in `evidence/g0/gpgx-capabilities.json`
of the Tessera specification (TP-013.2). A bit enters the mask only with a new
minor version of the contract; `MODE4_CONTROLS` is a candidate when SMS, Game
Gear or SG-1000 arrive (phase 6).

## Test vectors

The eight vectors are shared by the four products. A product passes the contract
when it produces, for each vector, the verdict, the reason and the exit code
below.

| Vector | `verdict` | `reason` | Exit |
|---|---|---|---|
| gpgx r4 x64 | `ok` | — | 0 |
| `ayther_test_core` | `ok` | — | 0 |
| non-existent path | `missing_file` | `core.missing` | 2 |
| `genesis_plus_gx_libretro_ayther_x86.dll` | `wrong_architecture` | `core.wrong_architecture` | 2 |
| a text file | `not_a_core` | `core.invalid` | 3 |
| `ayther_test_core_stock` | `abi_missing` | `core.abi_missing` | 4 |
| `ayther_test_core_partial` | `capabilities_incomplete` | `core.capabilities_incomplete` | 4 |
| `ayther_test_core_hang` | `timeout` | `core.probe_timeout` | 5 (set by the parent) |

## Compatibility

Consumers must ignore unknown JSON fields. A consumer that receives
`protocol_version` greater than the one it knows treats the answer as
`not_a_core` with the reason `core.invalid` and does not interpret the rest.
