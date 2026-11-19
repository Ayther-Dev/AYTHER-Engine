# Development environment setup

This guide prepares an AYTHER Engine development environment on x64 Windows or
x64 Linux and shows how to consume an installed Engine. Run project commands
from the repository root. Build and test commands, presets, and CI are in
[Build, test, and release](BUILD_TEST_RELEASE.md).

> [!NOTE]
> Headless presets build the Rust core and C ABI without native dependencies.
> Native presets build the complete engine and resolve SDL3, Vulkan, and the
> other C/C++ dependencies through vcpkg manifest mode. VP9 (`*-vpx`) and the
> Tessera provider are optional.

## Toolchain

| Tool | Required version or role |
|---|---|
| Git | Current supported release |
| CMake | 3.21 or later |
| Ninja | Generator used by every shared preset |
| LLVM/Clang | Windows: `clang-cl` 22 or later (CI pins 22.1.8). Linux: `clang`/`clang++` 18 or later (CI formats, analyzes, and measures coverage with 18) |
| clang-format | 18, to match the CI format check |
| Rust | 1.95.0, selected by `rust-toolchain.toml` |
| vcpkg | Required by native presets; unused by headless presets |
| PowerShell | 7 (`pwsh`) for project scripts |
| Vulkan driver/SDK | Driver required at runtime; SDK needed for GPU tools or shader rebuilding |

Windows also requires Visual Studio Build Tools 2022 or later with the
**Desktop development with C++** workload and a Windows SDK; `clang-cl` uses
their MSVC runtime, STL, and SDK. The MSVC 14.52 STL shipped with Visual
Studio 2026 refuses Clang older than 22 (error `STL1000`). Linux requires a
C/C++ linker and standard build tools.

## Windows

### 1. Install the host tools

Install Visual Studio Build Tools from the
[Visual Studio downloads page](https://visualstudio.microsoft.com/downloads/)
with the **Desktop development with C++** workload and a current Windows SDK.

With WinGet, install the command-line tools from a PowerShell terminal:

```powershell
winget install --id Git.Git --exact
winget install --id Kitware.CMake --exact
winget install --id Ninja-build.Ninja --exact
winget install --id Microsoft.PowerShell --exact
```

Install LLVM in the default `C:\Program Files\LLVM` location; the Windows
presets name the compiler, linker, resource compiler, and manifest tool there.
`tools/install_pinned_llvm.ps1` installs the version CI uses (22.1.8, through
Chocolatey) and verifies it:

```powershell
pwsh tools/install_pinned_llvm.ps1
```

Install Rust through the official
[rustup installer](https://www.rust-lang.org/tools/install). The repository's
`rust-toolchain.toml` selects Rust 1.95.0 with `rustfmt`, `clippy`, and
`rust-src`; do not set a repository-specific rustup override.

For renderer and GPU presets, install the current
[LunarG Vulkan SDK](https://vulkan.lunarg.com/sdk/home) and a Vulkan-capable GPU
driver. Restart the terminal so `VULKAN_SDK` and the updated `PATH` are
visible.

### 2. Prepare vcpkg

Keep vcpkg outside the Engine checkout:

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

The assignment updates the current terminal; the user variable is available to
new terminals. Any installation directory works as long as `VCPKG_ROOT` points
to it.

Do not run `vcpkg integrate install`. The native presets select
`$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake` explicitly, and
`vcpkg.json` with its `builtin-baseline` defines the direct dependencies and
port revisions. CMake selects the `renderer` feature automatically.

### 3. Clone and activate the Rust toolchain

```powershell
git clone https://github.com/Ayther-Dev/AYTHER-Engine.git
Set-Location AYTHER-Engine
rustup toolchain install 1.95.0 --profile minimal --component rustfmt --component clippy --component rust-src
rustup show active-toolchain
```

### 4. Verify the Windows environment

```powershell
git --version
cmake --version
ninja --version
clang-cl --version
rustc --version
pwsh --version
cmake --list-presets
```

Confirm CMake 3.21 or later, `clang-cl` 22 or later, Rust 1.95.0, that
`VCPKG_ROOT` is set, and that CMake lists the `windows-*` presets.

For VP9, also install NASM and GNU Make (from Scoop or MSYS2) and build libvpx
once:

```powershell
winget install --id NASM.NASM --exact
scoop install make
pwsh tools/build_libvpx.ps1
```

The script pins libvpx `v1.15.2`, builds only the static VP9 decoder, and puts
its headers, `vpxmd.lib`, version, license, patent grant, and authors under the
ignored `third_party/libvpx/`. Use `-Clean` after moving the checkout or
changing the tag.

## Linux

The commands target apt-based distributions; use equivalent packages elsewhere.

### 1. Install the host tools

```bash
sudo apt update
sudo apt install -y build-essential git cmake ninja-build curl zip unzip tar \
  pkg-config autoconf autoconf-archive automake libtool
sudo apt install -y clang-18 lld-18 clang-format-18 clang-tidy-18
```

If those packages are unavailable, use [apt.llvm.org](https://apt.llvm.org/).

Install Rust with rustup:

```bash
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
source "$HOME/.cargo/env"
```

Install PowerShell 7 with Microsoft's
[Ubuntu instructions](https://learn.microsoft.com/en-us/powershell/scripting/install/install-ubuntu).

For renderer and GPU presets, install the
[LunarG Vulkan SDK](https://vulkan.lunarg.com/sdk/home), a Vulkan driver, and
the development packages SDL needs; the
[SDL Linux guide](https://wiki.libsdl.org/SDL3/README-linux) lists them. SDL's
X11 backend is compiled only if the X11 development headers are present when
vcpkg builds SDL. For `linux-native-vpx`, install the distribution's libvpx
development package (for example `libvpx-dev`); the Linux VPX variant uses it
through `pkg-config` instead of vcpkg.

### 2. Prepare vcpkg

```bash
mkdir -p "$HOME/tools"
git clone https://github.com/microsoft/vcpkg.git "$HOME/tools/vcpkg"
"$HOME/tools/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
export VCPKG_ROOT="$HOME/tools/vcpkg"
```

Add the `VCPKG_ROOT` export to your shell profile. Do not run a global
integration command.

### 3. Clone and activate the Rust toolchain

```bash
git clone https://github.com/Ayther-Dev/AYTHER-Engine.git
cd AYTHER-Engine
rustup toolchain install 1.95.0 --profile minimal \
  --component rustfmt --component clippy --component rust-src
rustup show active-toolchain
```

### 4. Make Clang available to CMake

The Linux presets invoke `clang` and `clang++`. If the distribution exposes only
versioned commands such as `clang-18`, create an ignored
`CMakeUserPresets.json` in the repository root:

```json
{
  "version": 3,
  "configurePresets": [
    {
      "name": "linux-native-local",
      "inherits": "linux-native",
      "cacheVariables": {
        "CMAKE_C_COMPILER": "clang-18",
        "CMAKE_CXX_COMPILER": "clang++-18"
      }
    }
  ],
  "buildPresets": [
    { "name": "linux-native-local", "configurePreset": "linux-native-local" }
  ],
  "testPresets": [
    { "name": "linux-native-local", "configurePreset": "linux-native-local" }
  ]
}
```

Keep machine-specific compiler paths and environment values in
`CMakeUserPresets.json` and never commit it. A one-off alternative is passing
`-DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18` to
`cmake --preset`, as the CI coverage job does.

### 5. Verify the Linux environment

```bash
git --version
cmake --version
ninja --version
clang --version
clang-format-18 --version
rustc --version
pwsh --version
cmake --list-presets
```

Confirm CMake 3.21 or later, Clang 18 or later, Rust 1.95.0, and that CMake
lists the `linux-*` presets.

## Native dependencies

The native build resolves dependencies through CMake targets rather than
attaching include paths to `ayther_engine`:

| Dependency | Target used by AYTHER | Visibility |
|---|---|---|
| SDL3 | `SDL3::SDL3` (`Ayther::sdl3` adapter when installed) | Private; no installed header exposes SDL types |
| Vulkan | `Vulkan::Vulkan` | Public: `engine/vulkan_interop.hpp` exposes Vulkan types |
| Vulkan Memory Allocator | `GPUOpen::VulkanMemoryAllocator` | Private |
| vk-bootstrap | `vk-bootstrap::vk-bootstrap` | GPU tests only; production context creation belongs to Runtime |
| stb | `Stb::Stb` compatibility target | Private, build tree only |
| dr_libs | `dr_libs::dr_libs` compatibility target | Private, build tree only |
| toml++ | `tomlplusplus::tomlplusplus` | Private |
| zstd | `zstd::libzstd` | Private |
| libvpx | `Ayther::vpx` | Only with `AYTHER_ENABLE_VPX=ON` |
| Tessera | `Tessera::core` | Only with `AYTHER_ENABLE_TESSERA=ON`; see [Tessera native provider](TESSERA_NATIVE.md) |

The vcpkg modules for stb and dr_libs expose include-directory variables only.
AYTHER wraps each in an imported interface target linked through a
`BUILD_INTERFACE`, so they do not leak into the installed package.
Configuration fails if an expected target is missing.

The GLSL sources in `shaders/` and their precompiled SPIR-V are registered as
private `ayther_engine` resources and the SPIR-V is installed under
`share/Ayther/shaders`.

## Consuming an installed engine

Engine consumers must make SDL3, Vulkan, VulkanMemoryAllocator, toml++, and
zstd discoverable, typically by declaring the same packages in their own vcpkg
manifest. Applications that create the Vulkan context, such as Runtime, also
depend on vk-bootstrap directly. `find_package(Ayther)` resolves these packages
and verifies their imported targets before loading `Ayther::engine`; stb and
dr_libs are compiled in and not required from consumers. A package built with
the Tessera provider also requires the matching Tessera package
(`find_dependency(Tessera CONFIG)`) and sets `Ayther_HAS_TESSERA`.

```cmake
find_package(Ayther 0.1.0 CONFIG REQUIRED COMPONENTS engine)
target_link_libraries(my_app PRIVATE Ayther::engine)
```

Point `CMAKE_PREFIX_PATH` at the AYTHER install prefix. The installed header
surface is the allowlist in `cmake/AytherPublicHeaders.txt`, indexed in
[Public API index](PUBLIC_API_INDEX.md); renderer, audio implementation,
libretro-host, and Vulkan-backend headers stay internal.

`Ayther_SHADER_DIR`, exported by packages with the `engine` component, is the
only supported location of the installed SPIR-V. Do not reconstruct the package
layout or refer to an Engine checkout. A target can stage the shaders beside
its executable with:

```cmake
add_custom_command(TARGET runtime POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${Ayther_SHADER_DIR}"
        "$<TARGET_FILE_DIR:runtime>/shaders"
    VERBATIM)
```

A Windows VPX package is self-contained for VPX and exports `Ayther::vpx`; the
non-VPX package keeps video decoding disabled. `tests/package_consumer/` is the
permanent smoke consumer for package discovery, the public C and C++ facades,
transitive linking, execution, and the SDK version.

## Troubleshooting

### Configuring a native preset reports that Ninja cannot be found

The usual cause is an unset `VCPKG_ROOT`, not a missing Ninja. When the
toolchain file cannot be resolved, CMake fails while probing the compiler and
names the generator it never got to use. Check `VCPKG_ROOT` first:

```powershell
$env:VCPKG_ROOT
```

Headless presets do not use vcpkg, so a successful `windows-headless`
configure is not evidence that `VCPKG_ROOT` is set.

### The vcpkg toolchain file does not exist

Check that `VCPKG_ROOT` points to the bootstrapped vcpkg directory, which
contains the executable and `scripts/`.

### `STL1000` or "unexpected compiler version" on Windows

The MSVC STL is newer than the installed Clang. Run
`pwsh tools/install_pinned_llvm.ps1`, or install LLVM 22 or later in
`C:\Program Files\LLVM`.

### CMake cannot find `clang-cl`, `clang`, or `clang++`

Windows presets expect `C:\Program Files\LLVM\bin`. On Linux, use an ignored
local preset or `-DCMAKE_CXX_COMPILER` when compiler commands carry a version
suffix.

### The CI format check fails on code that is formatted locally

CI runs clang-format 18 over every changed file. Newer clang-format releases
format some constructs differently; reformat the changed files with version 18.

### Windows headers or libraries are missing

Add the **Desktop development with C++** workload and a Windows SDK to the
Visual Studio Build Tools installation. LLVM alone does not provide them.

### Vulkan configuration or GPU tests fail

Use a headless preset when graphics are not needed. For renderer work, check
that the Vulkan SDK environment is active and that `vulkaninfo` detects a
driver. GPU tests need a physical or virtual Vulkan device.

### A build directory contains stale settings

Each preset owns one directory under `build/`. Remove only the affected
`build/<preset>` directory and configure again.

### Linking fails with `LNK1104` naming a Cargo build script

The checkout path is too long. Windows limits paths to 260 characters unless
long paths are enabled, and Cargo's intermediate directories are deep. Clone
closer to the drive root or enable long paths.
