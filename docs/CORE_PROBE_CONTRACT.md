# Core probe contract (v2)

Contract shared by the Engine, the Runtime, the Lab and Play CE to decide whether
a libretro core on disk can be used with AYTHER. It extends `probe_core`
(`include/ayther/engine/core_probe.hpp`), which only reads the libretro
information entry points, with the AYTHER ABI and the capability check
(`probe_core_v2`).

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
| `capabilities_incomplete` | `core.capabilities_incomplete` | 4 | AYTHER ABI present, but older than 1.10 or `capabilities` does not cover `required_capabilities`. |
| `timeout` | `core.probe_timeout` | 5 | The probe did not answer in time. The parent process sets this verdict; the child never reports it. |
| `crashed` | `core.probe_crashed` | 6 | The probe ended without a valid answer (a crash, a signal, no status line). The parent process sets this verdict; the child never reports it. |

Exit code 1 is reserved for a usage error of the probe command itself.

The probe runs in a child process so that a core that hangs or crashes at load
time cannot take the caller down. See [Integration guide](#integration-guide).

## The command

The Engine installs the probe as `bin/ayther_core_probe` (`.exe` on Windows),
with no dependency on SDL, Vulkan or any other Engine library:

```
ayther_core_probe <core-path>
```

It prints one `AYTHER_STATUS` line and exits with the code of the verdict (0, 2,
3 or 4; 1 for a usage error and no line). The Runtime exposes the same output
with `--probe-core`. Inside a C++ program that is already isolated in its own
process, `ayther::engine::probe_core_v2()` returns the same data as a value.

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
  "reason":"core.capabilities_incomplete","api":1,"library_name":"AYTHER Test Core",
  "library_version":"ayther-test-core-1","architecture":"x64","ayther_abi":"1.10",
  "ayther_build_id":"ayther-test-core-1","capabilities":"0x6FFFF",
  "required_capabilities":"0xEFFFF","missing_capabilities":["SPRITE_OUTCOME_V1"]}
```

(The examples are wrapped for reading; the real line has no line breaks.)

Fields:

- `ok`, `verdict`: always present. `reason` appears only when `ok` is false.
- `api`, `library_name`, `library_version`: from the libretro information entry
  points, present once the library loaded. `architecture`: `x64`, `x86` or
  `arm64`, present when the file was read as a binary of this host or was
  rejected for its architecture.
- `ayther_abi`, `ayther_build_id`: from the AYTHER extension, present when it
  exists. `ayther_abi` is `major.minor`.
- `capabilities`: the bit mask the core announces (`AYTHER_CAP_*` of
  `ayther_api.h`, hexadecimal string). `required_capabilities`: the mask the
  Engine requires. `missing_capabilities`: the names of the required bits that
  are not announced, in bit order (without the `AYTHER_CAP_` prefix). The three
  appear together with `ayther_abi`.
- `message`: free text for logs; never parsed and never shown as a localized
  message. Consumers localize from `verdict` and `reason`.

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

An AYTHER interface older than 1.10 is `capabilities_incomplete` even if its
mask happens to cover the required bits.

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

The package publishes them in `share/ayther/probe-vectors/`:

- `probe-vectors.json`: the eight vectors with their expected answers. A vector
  with `library` names a core of this directory (append the platform library
  suffix: `.dll`, `.so`); one with `file` names a literal file; one with
  `external` is provided by the product that runs it (gpgx r4 and the x86
  library are binaries of the products, not of the Engine). Compare `verdict`,
  `reason`, `exit_code` and, when present, `ayther_abi`, `capabilities` and
  `missing_capabilities`; never compare `message`.
- `ayther_test_core`, `ayther_test_core_stock`, `ayther_test_core_partial` and
  `ayther_test_core_hang`: the fixture cores. `ayther_test_core_hang` never
  returns from `retro_init`; run it only through a supervising parent.
- `not-a-core.txt`: the text-file vector.

The Engine checks the vectors in three places: `ayther.contract.core_probe_vectors`
(the six that run in process), the package consumer
(`tests/package_consumer/check_core_probe.cmake`, the installed command over the
installed vectors, including `timeout` through a supervising parent) and
`check_release_payload.ps1` (the files exist in the package).

## Integration guide

Every product runs the probe the same way: as a child process, with a time
limit, never in its own process. A core is native code that runs in `retro_init`;
a hostile or broken one can hang, crash or corrupt the caller.

| Product | Child process | Parent |
|---|---|---|
| Lab | `ayther_core_probe <core>` from the Engine package | Lab |
| Runtime | itself, `--probe-core <core>` | the caller (Play CE) |
| Play CE | `ayther_runtime --probe-core <core>` | Play CE |

### Rules for the parent

1. **Time limit: 10 seconds.** Start the child, wait at most 10 s of wall clock.
   The limit is configurable by the caller for tests (the package consumer uses
   2 s for the hang vector) but a product must not raise it above 10 s.
2. **On expiry, kill the child and its descendants** (a job object on Windows, a
   process group on POSIX) and set the verdict `timeout`, reason
   `core.probe_timeout`, exit code 5. Do not wait for the child to finish after
   killing it indefinitely: reap it with a short second limit.
3. **Read standard output, bounded** (64 KiB is far more than a status line),
   with no standard input. Standard error is for logs.
4. **When the child exits in time,** take the last line that starts with
   `AYTHER_STATUS ` and parse its JSON.
   - A valid line whose exit code matches the verdict (the table above) is the
     answer.
   - A `protocol_version` greater than the one the parent knows is
     `not_a_core` with reason `core.invalid`; the rest is not interpreted.
   - **No valid line, or an exit code that does not belong to the verdict** (a
     crash exit status such as `0xC0000005`, a signal, exit 1, exit 5 or 6
     reported by the child itself) is `crashed`, reason `core.probe_crashed`,
     exit code 6.
5. **The parent decides `timeout` and `crashed`.** The child never reports them;
   a line that says so is treated as `crashed`.
6. **Validate again when it matters.** The answer is about one file at one moment:
   key a cache on path, size and modification time, and probe again before
   starting a session with that core (RF-9.3).
7. **Localize from `verdict` and `reason`,** not from `message`.

### Pseudocode

```text
fn probe(core_path, limit = 10 s) -> ProbeVerdict:
    if not exists(core_path): return missing_file        # optional shortcut
    child = spawn(["ayther_core_probe", core_path],
                  stdin = null, stdout = pipe, stderr = pipe,
                  group = new_kill_group())
    if not child.wait(limit):
        child.kill_group()
        child.wait(2 s)
        return timeout                                   # exit 5, core.probe_timeout
    line = last_line_starting_with("AYTHER_STATUS ", child.stdout[..64 KiB])
    if line is none:
        return crashed                                   # exit 6, core.probe_crashed
    status = parse_json(line after the prefix)
    if status is invalid or status.protocol_version > KNOWN_PROTOCOL:
        return not_a_core                                # core.invalid
    if status.verdict in [timeout, crashed]:
        return crashed                                   # the child must not say it
    if child.exit_code != exit_code_of(status.verdict):
        return crashed
    return status
```

In Rust (Play CE) the same shape is a `std::process::Command` with piped output
and a wait with deadline; on expiry call `Child::kill` on the job or process
group and then `wait` to reap it. In C++ (Lab) it is `CreateProcessW` into a job
object with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` and `WaitForSingleObject(limit)`.

## Compatibility

Consumers must ignore unknown JSON fields. A consumer that receives
`protocol_version` greater than the one it knows treats the answer as
`not_a_core` with the reason `core.invalid` and does not interpret the rest.
