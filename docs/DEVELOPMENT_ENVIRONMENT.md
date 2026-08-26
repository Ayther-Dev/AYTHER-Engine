# Development environment setup

This guide prepares a reproducible AYTHER Engine development environment on
x64 Windows or x64 Linux. Run project commands from the repository root unless
the guide says otherwise.

> [!NOTE]
> Complete the toolchain installation and verification before configuring. If
> the checkout does not contain a root `CMakeLists.txt`, stop after the
> verification section; configure, build, test, and install require the build
> targets to be present.

## Toolchain

| Tool | Required version or role |
|---|---|
| Git | Current supported release |
| CMake | 3.21 or later |
| Ninja | Build executor used by every shared preset |
| LLVM/Clang | Version 18; `clang-cl` on Windows and `clang`/`clang++` on Linux |
| Rust | 1.95.0, selected by `rust-toolchain.toml` |
| vcpkg | C++ dependency manager; port versions are pinned by `vcpkg.json` |
| PowerShell | Version 7 (`pwsh`) for project scripts |
| Vulkan SDK | Required when building the renderer or GPU tests |

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

Install LLVM 18 from the
[LLVM 18.1.8 release](https://github.com/llvm/llvm-project/releases/tag/llvmorg-18.1.8)
and make its `bin` directory available on `PATH`. The shared Windows presets
invoke `clang-cl` by name.

Install Rust through the official
[rustup installer](https://www.rust-lang.org/tools/install). The repository's
`rust-toolchain.toml` selects Rust 1.95.0 and requests `rustfmt`, `clippy`, and
`rust-src`; do not set a repository-specific rustup override.

For renderer and GPU presets, install the current
[LunarG Vulkan SDK](https://vulkan.lunarg.com/sdk/home) and a Vulkan-capable GPU
driver. Restart the terminal after installation so `VULKAN_SDK` and the updated
`PATH` are visible.

### 2. Install vcpkg

Keep vcpkg outside the AYTHER Engine checkout:

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

Do not run `vcpkg integrate install`. The CMake presets already select
`$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake`, and the repository uses
vcpkg manifest mode.

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

Run these checks from a Visual Studio Developer PowerShell so `clang-cl`
can find the MSVC libraries and Windows SDK:

```powershell
git --version
cmake --version
ninja --version
clang-cl --version
rustc --version
cargo --version
pwsh --version
& "$env:VCPKG_ROOT\vcpkg.exe" version
Test-Path "$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake"
cmake --list-presets
```

Confirm that CMake is at least 3.21, Clang reports major version 18, Rust reports
1.95.0, the `Test-Path` result is `True`, and the command lists the four Windows
presets.

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

### 2. Install vcpkg

```bash
mkdir -p "$HOME/tools"
git clone https://github.com/microsoft/vcpkg.git "$HOME/tools/vcpkg"
"$HOME/tools/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
export VCPKG_ROOT="$HOME/tools/vcpkg"
```

Add the `VCPKG_ROOT` export to the appropriate shell profile to make it
persistent. Do not run a global integration command; the shared CMake presets
already point to the vcpkg toolchain file.

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
"$VCPKG_ROOT/vcpkg" version
test -f "$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --list-presets
```

Confirm that CMake is at least 3.21, Clang reports major version 18, Rust reports
1.95.0, the `test` command succeeds, and CMake lists the four Linux presets.

## Shared CMake presets

Replace `<platform>` with `windows` or `linux`.

| Preset | Build type | Renderer | CPU tests and examples | GPU tests | Intended use |
|---|---|---:|---:|---:|---|
| `<platform>-headless` | RelWithDebInfo | No | Yes | No | Fast local checks and CI without graphics |
| `<platform>-dev` | RelWithDebInfo | Yes | Yes | No | Normal renderer development |
| `<platform>-gpu` | RelWithDebInfo | Yes | Yes | Yes | Validation on a Vulkan-capable GPU |
| `<platform>-release` | Release | Yes | No | No | Optimized distributable build |

The first configure run restores the dependencies declared in `vcpkg.json`
into the ignored `vcpkg_installed/` directory. The manifest's
`builtin-baseline` pins the registry state used to resolve those dependencies.

Once the root build targets are available, a headless Windows workflow is:

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
development or headless tests before producing a release build.

## Troubleshooting

### The vcpkg toolchain file does not exist

Check that `VCPKG_ROOT` is defined in the current terminal and points to the
directory containing the bootstrapped vcpkg executable and `scripts/` folder.
Open a new terminal after setting a persistent Windows environment variable.

### CMake cannot find `clang-cl`, `clang`, or `clang++`

Verify the selected compiler is LLVM 18 and available on `PATH`. On Windows,
run from a Visual Studio Developer PowerShell. On Linux, use an ignored local
preset when the compiler commands have a version suffix.

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
