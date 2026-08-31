# Development environment setup

This guide prepares a reproducible AYTHER Engine development environment on
x64 Windows or x64 Linux. Run project commands from the repository root unless
the guide says otherwise.

> [!NOTE]
> Complete the toolchain installation and verification before configuring. The
> root `CMakeLists.txt` supports a dependency-free headless core build and a
> complete 24-source native engine build. Native presets select vcpkg manifest
> mode and expose `Ayther::engine`; VPX remains an explicit optional step.

## Toolchain

| Tool | Required version or role |
|---|---|
| Git | Current supported release |
| CMake | 3.21 or later |
| Ninja | Build executor used by every shared preset |
| LLVM/Clang | Version 18 or later; `clang-cl` on Windows and `clang`/`clang++` on Linux |
| Rust | 1.95.0, selected by `rust-toolchain.toml` |
| vcpkg | Required by native presets; unused by headless/core presets |
| PowerShell | Version 7 (`pwsh`) for project scripts |
| Vulkan driver/SDK | Driver required at runtime; SDK needed for GPU tools or shader rebuilding |

Windows also requires the Visual Studio Build Tools 2022 or later with the
**Desktop development with C++** workload and a Windows SDK. Linux requires a
C/C++ linker and standard build tools.

## Windows

### 1. Install the host tools

Install the Visual Studio Build Tools 2022 or later from the
[Visual Studio downloads page](https://visualstudio.microsoft.com/downloads/).
Select the **Desktop development with C++** workload and a current Windows SDK.
The `x64-windows` vcpkg triplet and `clang-cl` use this MSVC-compatible runtime
and SDK.

On Windows clients with WinGet, install the common command-line tools from a
PowerShell terminal:

```powershell
winget install --id Git.Git --exact
winget install --id Kitware.CMake --exact
winget install --id Ninja-build.Ninja --exact
winget install --id Microsoft.PowerShell --exact
```

Install LLVM 18 or later from the
[LLVM 18.1.8 release](https://github.com/llvm/llvm-project/releases/tag/llvmorg-18.1.8)
or a newer supported release. Install it in the default
`C:\Program Files\LLVM` location; the shared Windows presets name the compiler,
linker, resource compiler, and manifest tool there explicitly.

Install Rust through the official
[rustup installer](https://www.rust-lang.org/tools/install). The repository's
`rust-toolchain.toml` selects Rust 1.95.0 and requests `rustfmt`, `clippy`, and
`rust-src`; do not set a repository-specific rustup override.

For renderer and GPU presets, install the current
[LunarG Vulkan SDK](https://vulkan.lunarg.com/sdk/home) and a Vulkan-capable GPU
driver. Restart the terminal after installation so `VULKAN_SDK` and the updated
`PATH` are visible.

### 2. Prepare vcpkg for native-engine work

Headless/core presets do not require vcpkg. Native presets do. Keep vcpkg
outside the AYTHER Engine checkout:

```powershell
$vcpkgInstallDir = Join-Path $env:USERPROFILE "tools\vcpkg"
New-Item -ItemType Directory -Path (Split-Path $vcpkgInstallDir) -Force | Out-Null
git clone https://github.com/microsoft/vcpkg.git $vcpkgInstallDir
& "$vcpkgInstallDir\bootstrap-vcpkg.bat" -disableMetrics

$env:VCPKG_ROOT = $vcpkgInstallDir
[Environment]::SetEnvironmentVariable(
    "VCPKG_ROOT",
    $env:VCPKG_ROOT,
    [EnvironmentVariableTarget]::User
)
```

The assignment updates the current terminal; the persistent user variable is
available to newly opened terminals. A different installation directory is
valid as long as `VCPKG_ROOT` points to it.

Do not run `vcpkg integrate install`. The native presets select
`$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake` explicitly. `vcpkg.json`
and its `builtin-baseline` define the direct dependencies and exact port
revisions; the `renderer` feature is selected automatically by CMake.

The native build resolves dependencies through CMake targets rather than
attaching package include paths directly to `ayther_engine`:

| Dependency | Target used by AYTHER | Visibility |
|---|---|---|
| SDL3 | `SDL3::SDL3` | Private link dependency; no installed header exposes SDL types |
| Vulkan | `Vulkan::Vulkan` | Private link dependency; no installed header exposes Vulkan types |
| Vulkan Memory Allocator | `GPUOpen::VulkanMemoryAllocator` | Private |
| vk-bootstrap | `vk-bootstrap::vk-bootstrap` | Private |
| stb | `Stb::Stb` compatibility target | Private, build-tree only |
| dr_libs | `dr_libs::dr_libs` compatibility target | Private, build-tree only |
| toml++ | `tomlplusplus::tomlplusplus` | Private |
| zstd | `zstd::libzstd` | Private |

The vcpkg modules for stb and dr_libs currently expose include-directory
variables only. AYTHER wraps each one in an imported interface target and links
those targets through a `BUILD_INTERFACE`, so they do not leak into the
installed package. Configuration fails immediately if any expected target is
missing.

`ayther_engine` uses `include/ayther/` and `src/` as private source-tree include
roots. Third-party include directories are not repeated there: they arrive from
the dependency targets above. Its private compile contract defines
`VMA_STATIC_VULKAN_FUNCTIONS=1`, `VMA_DYNAMIC_VULKAN_FUNCTIONS=0`, and, only
when `AYTHER_ENABLE_VPX=ON`, `AYTHER_HAVE_VPX=1`. VPX is linked only in that
configuration; core, ymfm, Threads, VMA, vk-bootstrap, stb, dr_libs, toml++,
zstd, and `${CMAKE_DL_LIBS}` form the remaining private link closure. SDL3 and
Vulkan are public because installed AYTHER headers expose their types.

The eight GLSL sources and their eight precompiled SPIR-V counterparts are
registered as private `ayther_engine` resources. CMake marks them
`HEADER_FILE_ONLY`, groups them under `Shaders` in IDE generators, and installs
the SPIR-V list under `share/Ayther/shaders` without relying on a directory
glob.

### 3. Clone and activate the Rust toolchain

```powershell
git clone https://github.com/Ayther-Dev/AYTHER-Engine.git
Set-Location AYTHER-Engine
rustup toolchain install 1.95.0 --profile minimal --component rustfmt --component clippy --component rust-src
rustup show active-toolchain
```

The install command provisions the pinned toolchain and components. The last
command confirms that the repository selects it.

### 4. Verify the Windows environment

Run these checks from PowerShell. Visual Studio Build Tools still supplies the
MSVC runtime and Windows SDK used by `clang-cl`:

```powershell
git --version
cmake --version
ninja --version
clang-cl --version
rustc --version
cargo --version
pwsh --version
cmake --list-presets
```

Confirm that CMake is at least 3.21, Clang is 18 or later, Rust reports 1.95.0,
`VCPKG_ROOT` is set, and CMake lists `windows-headless`, `windows-native`, and
the `windows-native-vpx` and `windows-native-gpu` variants.

## Linux

The commands below target apt-based distributions. Use equivalent package names
on other distributions.

### 1. Install the host tools

```bash
sudo apt update
sudo apt install -y build-essential git cmake ninja-build curl zip unzip tar \
  pkg-config autoconf autoconf-archive automake libtool
```

Install Clang 18 and LLD 18 from the distribution repository when available:

```bash
sudo apt install -y clang-18 lld-18
```

If those packages are unavailable, follow the instructions at
[apt.llvm.org](https://apt.llvm.org/) for your distribution.

Install Rust with rustup:

```bash
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
source "$HOME/.cargo/env"
```

Install PowerShell 7 using Microsoft's
[Ubuntu installation instructions](https://learn.microsoft.com/en-us/powershell/scripting/install/install-ubuntu).

For renderer and GPU presets, install the current
[LunarG Vulkan SDK](https://vulkan.lunarg.com/sdk/home), a Vulkan-capable GPU
driver, and the platform development packages required by SDL. The
[SDL Linux guide](https://wiki.libsdl.org/SDL3/README-linux) lists the optional
audio, display, and input development packages.

### 2. Prepare vcpkg for native-engine work

```bash
mkdir -p "$HOME/tools"
git clone https://github.com/microsoft/vcpkg.git "$HOME/tools/vcpkg"
"$HOME/tools/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
export VCPKG_ROOT="$HOME/tools/vcpkg"
```

Add the `VCPKG_ROOT` export to the appropriate shell profile. Native presets
require it; do not run a global integration command.

### 3. Clone and activate the Rust toolchain

```bash
git clone https://github.com/Ayther-Dev/AYTHER-Engine.git
cd AYTHER-Engine
rustup toolchain install 1.95.0 --profile minimal \
  --component rustfmt --component clippy --component rust-src
rustup show active-toolchain
```

The install command provisions the pinned toolchain and components. The last
command confirms that the repository selects it.

### 4. Make Clang 18 available to CMake

The shared Linux presets invoke `clang` and `clang++`. If those commands already
resolve to version 18, no customization is needed. If the distribution exposes
only `clang-18` and `clang++-18`, create an ignored `CMakeUserPresets.json` in
the repository root:

```json
{
  "version": 3,
  "configurePresets": [
    {
      "name": "linux-headless-local",
      "inherits": "linux-headless",
      "cacheVariables": {
        "CMAKE_C_COMPILER": "clang-18",
        "CMAKE_CXX_COMPILER": "clang++-18"
      }
    }
  ],
  "buildPresets": [
    {
      "name": "linux-headless-local",
      "configurePreset": "linux-headless-local"
    }
  ],
  "testPresets": [
    {
      "name": "linux-headless-local",
      "configurePreset": "linux-headless-local",
      "output": {
        "outputOnFailure": true
      },
      "execution": {
        "noTestsAction": "error"
      }
    }
  ]
}
```

Keep machine-specific compiler paths and environment values in
`CMakeUserPresets.json`; never commit that file.

### 5. Verify the Linux environment

```bash
git --version
cmake --version
ninja --version
clang-18 --version
rustc --version
cargo --version
pwsh --version
cmake --list-presets
```

Confirm that CMake is at least 3.21, Clang is 18 or later, Rust reports 1.95.0,
and CMake lists the Linux headless, native, native-VPX, and native-GPU presets.

## Shared CMake presets

Replace `<platform>` with `windows` or `linux`.

| Preset | Build type | Tests | Intended use |
|---|---|---:|---|
| `<platform>-headless` | RelWithDebInfo | Yes | Core, bridge, and flat-ABI validation |
| `<platform>-release` | Release | No | Optimized pre-release core package |
| `<platform>-native` | RelWithDebInfo | Yes | Complete 24-source engine with manifest dependencies |
| `<platform>-native-vpx` | RelWithDebInfo | Yes | Native engine plus VP9 decoding |
| `<platform>-native-gpu` | RelWithDebInfo | Yes | Native engine plus eight Vulkan GPU oracles |

Headless and release presets omit native dependencies. Native presets select
the vcpkg toolchain and its `renderer` feature.

The headless Windows workflow is:

```powershell
cmake --preset windows-headless
cmake --build --preset windows-headless
ctest --preset windows-headless
```

The Linux equivalent is:

```bash
cmake --preset linux-headless
cmake --build --preset linux-headless
ctest --preset linux-headless
```

For an installable release build, configure and build the release preset, then
install from its generated build directory:

```text
cmake --preset <platform>-release
cmake --build --preset <platform>-release
cmake --install build/<platform>-release
```

Release presets intentionally do not have a test preset. Run the corresponding
headless tests before producing a release build.

## Native engine workflow

Windows without VPX:

```powershell
$env:VCPKG_ROOT = Join-Path $env:USERPROFILE "tools\vcpkg"
cmake --preset windows-native
cmake --build --preset windows-native
ctest --preset windows-native
cmake --install build/windows-native --prefix install/windows-native
```

This installs `Ayther::core`, `Ayther::engine`, and `Ayther::ymfm`, the explicit
public-header allowlist, precompiled shaders, CMake package files, and the
selected third-party license texts. The installed engine surface is
`ayther_sdk.h`, `ayther_session.h`, `ayther_sdk_version.h`, and the supporting
types those facades include; renderer, audio implementation, libretro-host, and
Vulkan-backend headers stay private to the source tree.

For VP9 on Windows, Git for Windows, NASM, GNU Make, and the Visual Studio C++
workload must be installed. The script accepts GNU Make from Scoop or MSYS2:

```powershell
winget install --id NASM.NASM --exact
scoop install make
pwsh tools/build_libvpx.ps1
cmake --preset windows-native-vpx
cmake --build --preset windows-native-vpx
ctest --preset windows-native-vpx
cmake --install build/windows-native-vpx --prefix install/windows-native-vpx
```

The libvpx script pins `v1.15.2`, builds only the static VP9 decoder, and puts
its headers, `vpxmd.lib`, version, license, patent grant, and authors under
`third_party/libvpx/`. That generated directory is intentionally ignored by
Git. Use `-Clean` after moving the checkout or changing the tag.

On Linux, `linux-native` uses vcpkg for the renderer stack. The VPX variant
uses the system pkg-config module, so install the distribution's libvpx
development package first (for example `libvpx-dev` on Debian/Ubuntu), then run
the equivalent `linux-native-vpx` configure/build/test commands.

To compile and run only the hardware-dependent Vulkan oracles, use:

```text
cmake --preset <platform>-native-gpu
cmake --build --preset <platform>-native-gpu
ctest --preset <platform>-native-gpu
```

The GPU test preset filters by the `gpu` label. The regular native preset keeps
these tests disabled, so machines without a Vulkan device retain a deterministic
CPU/integration test run.

## Consuming an installed engine

Engine consumers must make SDL3, Vulkan, VulkanMemoryAllocator, vk-bootstrap,
toml++, and zstd discoverable. The supported source workflow is to declare the
same packages in the consumer's vcpkg manifest and configure with its toolchain.
The AYTHER package does not silently copy those libraries into another project.
`find_package(Ayther)` resolves these packages and verifies their imported
targets before loading `Ayther::engine`; stb and dr_libs remain compiled-in,
private implementation dependencies and are not required from consumers.

```cmake
find_package(Ayther 0.1 CONFIG REQUIRED COMPONENTS engine)
target_link_libraries(my_app PRIVATE Ayther::engine)
```

Point `CMAKE_PREFIX_PATH` at the AYTHER install prefix. A VPX-enabled Windows
installation is self-contained for VPX and additionally exports
`Ayther::vpx`; the non-VPX package keeps video decoding disabled. The permanent
smoke consumer under `tests/package_consumer/` verifies package discovery,
the public C and C++ facades, transitive linking, execution, and SDK version.

## Troubleshooting

### The vcpkg toolchain file does not exist during native-engine work

This does not affect core presets. For native presets, check that `VCPKG_ROOT`
points to the bootstrapped vcpkg executable and `scripts/` folder.

### CMake cannot find `clang-cl`, `clang`, or `clang++`

Verify the selected compiler is LLVM 18 or later. Windows presets expect the
default `C:\Program Files\LLVM\bin` installation. On Linux, use an ignored
local preset when compiler commands have a version suffix.

### Windows headers or libraries are missing

Modify the Visual Studio Build Tools installation and add the **Desktop
development with C++** workload and a Windows SDK. LLVM alone does not provide
the MSVC-compatible Windows SDK used by `clang-cl` and `x64-windows`.

### Vulkan configuration or GPU tests fail

Use a headless preset when graphics are not required. For renderer work, verify
that the Vulkan SDK environment is active and that `vulkaninfo` detects a
current vendor driver. GPU tests require physical or virtual Vulkan support.

### A configure directory contains stale settings

Each preset owns exactly one directory under `build/`. Remove only the affected
`build/<preset>` directory, then configure that preset again. Do not place build
artifacts in the source tree.

### Configuring a native preset reports that Ninja cannot be found

The real cause is usually an unset `VCPKG_ROOT`, not a missing Ninja. When the
toolchain file cannot be resolved, CMake fails while probing the compiler and
reports the generator it never got to use, which sends people to reinstall a
tool that was present all along. Confirm `VCPKG_ROOT` first:

```powershell
$env:VCPKG_ROOT
```

If it is empty, set it as described above and configure again. Core presets are
unaffected, so `windows-headless` succeeding is not evidence that `VCPKG_ROOT`
is set.

### Linking fails with `LNK1104` naming a Cargo build script

The checkout path is too long. Windows limits a path to 260 characters unless
long paths are enabled, and Cargo's intermediate directories are deep enough
that a checkout roughly 250 characters down will push them past the limit. The
error names a file the linker cannot open, so it reads like a missing artifact
rather than a path-length problem.

Clone closer to the drive root, or enable long paths. This is a Windows limit
rather than a defect in the repository; a build that fails this way at a deep
path succeeds unchanged at a shallow one.
