<#
.SYNOPSIS
    Downloads a published AYTHER artifact and proves it is verifiable,
    installable, and consumable.

.DESCRIPTION
    This is the acceptance check for a published release, meant to be run from a
    CLEAN CHECKOUT of the tag on a machine that did not build it. It refuses to
    take any single record on trust: the checksum, the Sigstore bundle, and the
    GitHub attestation are all checked, then the archive is unpacked, its payload
    is verified against what its name advertises, and -- unless told otherwise --
    an out-of-tree CMake project links and runs against it.

    Requires the GitHub CLI (gh) and cosign on PATH. Consuming additionally
    requires a C++ toolchain, CMake, Ninja, and for the engine products a vcpkg
    environment supplying SDL3, Vulkan, VMA, toml++, and zstd.

.EXAMPLE
    ./tools/verify_release_artifact.ps1 -Tag v0.1.0-rc.1 -Product ayther-engine `
        -Platform linux-x86_64
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^v\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$')]
    [string]$Tag,

    [ValidateSet('ayther-core', 'ayther-engine', 'ayther-engine-vpx')]
    [string]$Product = 'ayther-engine',

    [ValidateSet('linux-x86_64', 'windows-x86_64')]
    [string]$Platform,

    [string]$WorkDirectory,

    [string]$Repository = 'Ayther-Dev/AYTHER-Engine',

    # Verification of the records only; skips the CMake configure/build/run.
    [switch]$SkipConsumer,

    # Point at a vcpkg toolchain for the engine products.
    [string]$ToolchainFile = $env:VCPKG_ROOT ?
        (Join-Path $env:VCPKG_ROOT 'scripts/buildsystems/vcpkg.cmake') : ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not $Platform) {
    $Platform = if ($IsWindows) { 'windows-x86_64' } else { 'linux-x86_64' }
}
if (-not $WorkDirectory) {
    $WorkDirectory = Join-Path ([System.IO.Path]::GetTempPath()) "ayther-verify-$Tag-$Product"
}

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$name = "$Product-$Tag-$Platform"
$archive = "$name.zip"

foreach ($tool in 'gh', 'cosign') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "Required tool '$tool' is not on PATH."
    }
}

if (Test-Path -LiteralPath $WorkDirectory) {
    Remove-Item -Recurse -Force -LiteralPath $WorkDirectory
}
New-Item -ItemType Directory -Path $WorkDirectory | Out-Null
$WorkDirectory = (Resolve-Path -LiteralPath $WorkDirectory).Path
Write-Host "=== verifying $archive from $Repository@$Tag"
Write-Host "    workspace: $WorkDirectory"

# --- 1. Download ----------------------------------------------------------
Push-Location $WorkDirectory
try {
    gh release download $Tag --repo $Repository `
        --pattern $archive `
        --pattern "$name.spdx.json" `
        --pattern "$archive.sigstore.json" `
        --pattern 'CHECKSUMS.sha256'
    if ($LASTEXITCODE -ne 0) { throw "Could not download '$archive' from $Tag." }
}
finally {
    Pop-Location
}
Write-Host '  [ OK ] downloaded archive, SBOM, signature bundle, and checksums'

# --- 2. Checksum ----------------------------------------------------------
$archivePath = Join-Path $WorkDirectory $archive
$expectedLine = Get-Content -LiteralPath (Join-Path $WorkDirectory 'CHECKSUMS.sha256') |
    Where-Object { $_ -like "* *$archive" }
if (-not $expectedLine) {
    throw "CHECKSUMS.sha256 does not list '$archive'."
}
$expected = ($expectedLine -split '\s+')[0]
$actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $archivePath).Hash.ToLowerInvariant()
if ($expected -ne $actual) {
    throw "Checksum mismatch for '$archive': expected $expected, got $actual."
}
Write-Host "  [ OK ] SHA-256 matches the published checksum"

# --- 3. Sigstore ----------------------------------------------------------
$identity = '^https://github.com/' + [regex]::Escape($Repository) +
    '/.github/workflows/release.yml@refs/tags/v[0-9].*$'
cosign verify-blob `
    --bundle "$archivePath.sigstore.json" `
    --certificate-oidc-issuer 'https://token.actions.githubusercontent.com' `
    --certificate-identity-regexp $identity `
    $archivePath
if ($LASTEXITCODE -ne 0) { throw "Cosign could not verify '$archive'." }
Write-Host '  [ OK ] Sigstore bundle verified against the release workflow identity'

# --- 4. SLSA provenance ---------------------------------------------------
gh attestation verify $archivePath --repo $Repository
if ($LASTEXITCODE -ne 0) { throw "Build provenance did not verify for '$archive'." }
Write-Host '  [ OK ] SLSA build provenance verified'

# --- 5. Unpack and check the payload -------------------------------------
$unpacked = Join-Path $WorkDirectory 'unpacked'
Expand-Archive -LiteralPath $archivePath -DestinationPath $unpacked
$prefix = Join-Path $unpacked $name
if (-not (Test-Path -LiteralPath $prefix)) {
    throw "The archive did not unpack to the expected root '$name'."
}

$kind = $Product -replace '^ayther-', ''
$payloadArguments = @{ Directory = $prefix; Kind = $kind }
if ($kind -eq 'engine-vpx' -and $Platform -eq 'windows-x86_64') {
    $payloadArguments['RequireBundledVpx'] = $true
}
& (Join-Path $repositoryRoot 'tools/check_release_payload.ps1') @payloadArguments

# --- 6. Consume -----------------------------------------------------------
if ($SkipConsumer) {
    Write-Host ''
    Write-Host "Records verified for '$archive'. Consumer skipped by request."
    return
}

$consumerSource = if ($kind -eq 'core') {
    Join-Path $repositoryRoot 'tests/package_consumer_core'
} else {
    Join-Path $repositoryRoot 'tests/package_consumer'
}
$consumerBuild = Join-Path $WorkDirectory 'consumer'
$executable = if ($kind -eq 'core') {
    'ayther_core_package_consumer'
} else {
    'ayther_package_consumer'
}
if ($Platform -eq 'windows-x86_64') { $executable += '.exe' }

$configure = @(
    '-S', $consumerSource,
    '-B', $consumerBuild,
    '-G', 'Ninja',
    '-DCMAKE_BUILD_TYPE=Release',
    '-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF',
    "-DCMAKE_PREFIX_PATH=$prefix"
)
# Only the engine products pull native dependencies; a core consumer that needed
# a toolchain file would mean the core package stopped being self-contained.
if ($kind -ne 'core' -and $ToolchainFile) {
    $configure += "-DCMAKE_TOOLCHAIN_FILE=$ToolchainFile"
}

cmake @configure
if ($LASTEXITCODE -ne 0) { throw 'The out-of-tree consumer failed to configure.' }
cmake --build $consumerBuild
if ($LASTEXITCODE -ne 0) { throw 'The out-of-tree consumer failed to build.' }

& (Join-Path $consumerBuild $executable)
if ($LASTEXITCODE -ne 0) { throw 'The out-of-tree consumer failed to run.' }

Write-Host ''
Write-Host "'$archive' is downloadable, verifiable, installable, and consumable."
