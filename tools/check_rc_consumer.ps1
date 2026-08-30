<#
.SYNOPSIS
    Installs a release candidate outside the source tree and drives it from an
    out-of-tree frontend, producing a reproducible report.

.DESCRIPTION
    The package-consumer gate in the release workflow proves an artifact LINKS.
    This proves it RUNS: it configures and builds the consumer against an
    installed prefix, then has it create a session, open a trusted pack, step
    frames, and report what the renderer and the audio device did.

    The report is checked for absolute paths into this repository before it is
    accepted. A report quoting the producer's build directory cannot be
    reproduced by whoever reads it, and it is also evidence that the artifact
    was consumed from the source tree instead of from an install -- which is the
    one thing this gate exists to rule out.

.EXAMPLE
    ./tools/check_rc_consumer.ps1 -Prefix ./unpacked/ayther-engine-v0.1.0-rc.1-windows-x86_64 `
        -Core ./bin/ayther_test_core.dll -Rom "$env:TEMP/ayther_synthetic.md"
#>
[CmdletBinding()]
param(
    # An installed or unpacked package prefix. NOT a build directory.
    [Parameter(Mandatory = $true)]
    [string]$Prefix,

    [Parameter(Mandatory = $true)]
    [string]$Core,

    [Parameter(Mandatory = $true)]
    [string]$Rom,

    [string]$Pack = '',
    [string]$TrustRegistry = '',

    [string]$ConsumerSource,
    [string]$WorkDirectory,
    [string]$ReportFile = 'rc-consumer-report.md',

    [string]$Compiler = '',
    [string]$ToolchainFile = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repositoryRoot = (Resolve-Path -LiteralPath (Split-Path -Parent $PSScriptRoot)).Path
if (-not $ConsumerSource) {
    $ConsumerSource = Join-Path $repositoryRoot 'tests/package_consumer'
}
if (-not $WorkDirectory) {
    $WorkDirectory = Join-Path ([System.IO.Path]::GetTempPath()) 'ayther-rc-consumer'
}
if (-not $ToolchainFile -and $env:VCPKG_ROOT) {
    $ToolchainFile = Join-Path $env:VCPKG_ROOT 'scripts/buildsystems/vcpkg.cmake'
}

$prefixPath = (Resolve-Path -LiteralPath $Prefix).Path
if ($prefixPath.StartsWith($repositoryRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
    # A prefix inside the checkout would let the consumer resolve headers and
    # libraries the package never installed, and pass without proving anything.
    throw ("The prefix '$prefixPath' is inside the repository. Install the " +
        'artifact somewhere else: consuming from the source tree does not ' +
        'demonstrate that the PACKAGE is consumable.')
}

if (Test-Path -LiteralPath $WorkDirectory) {
    Remove-Item -Recurse -Force -LiteralPath $WorkDirectory
}
New-Item -ItemType Directory -Path $WorkDirectory | Out-Null

Write-Host "=== release-candidate consumer ==="
Write-Host "  prefix:    $prefixPath"
Write-Host "  consumer:  $ConsumerSource"

# --- Configure and build the out-of-tree consumer -------------------------
$configure = @(
    '-S', $ConsumerSource,
    '-B', $WorkDirectory,
    '-G', 'Ninja',
    '-DCMAKE_BUILD_TYPE=Release',
    "-DCMAKE_PREFIX_PATH=$prefixPath"
)
if ($Compiler) { $configure += "-DCMAKE_CXX_COMPILER=$Compiler" }
if ($ToolchainFile) { $configure += "-DCMAKE_TOOLCHAIN_FILE=$ToolchainFile" }

& cmake @configure
if ($LASTEXITCODE -ne 0) { throw 'The consumer failed to configure against the installed package.' }
& cmake --build $WorkDirectory
if ($LASTEXITCODE -ne 0) { throw 'The consumer failed to build against the installed package.' }

$executable = Join-Path $WorkDirectory 'ayther_package_consumer'
if ($IsWindows) { $executable += '.exe' }
if (-not (Test-Path -LiteralPath $executable)) {
    throw "The consumer did not produce an executable at '$executable'."
}

# --- Run it ---------------------------------------------------------------
$arguments = @($Core, $Rom)
if ($Pack) { $arguments += $Pack } else { $arguments += '' }
if ($TrustRegistry) { $arguments += $TrustRegistry }

$output = & $executable @arguments 2>&1
$runExit = $LASTEXITCODE
$text = ($output | Out-String)
Write-Host $text

if ($runExit -ne 0) { throw "The consumer exited $runExit." }
if ($text -notmatch '=== consumer OK ===') {
    throw 'The consumer did not report success.'
}

# --- The report -----------------------------------------------------------
# Only the report block: the engine's own logging carries absolute paths that
# are legitimate at runtime but are not part of a reproducible record.
$reportLines = @()
$inReport = $false
foreach ($line in ($text -split "`r?`n")) {
    if ($line -match '^=== consumer report ===') { $inReport = $true; continue }
    if ($line -match '^=== consumer (OK|FAILED) ===') { $inReport = $false; continue }
    if (-not $inReport) { continue }
    $trimmed = $line.Trim()
    if (-not $trimmed) { continue }
    # The engine logs to the same stream, and its lines are interleaved with the
    # report. They are legitimate at runtime but they carry absolute paths, so
    # only the report's own `key: value` lines are kept.
    if ($trimmed.StartsWith('[')) { continue }
    if ($trimmed -notmatch '^[A-Za-z][A-Za-z0-9 _/+-]*:') { continue }
    $reportLines += $trimmed
}
if ($reportLines.Count -eq 0) { throw 'The consumer produced no report block.' }

$sdk = 'unknown'
$sdkMatch = [regex]::Match($text, 'AYTHER SDK (?<version>\S+)')
if ($sdkMatch.Success) { $sdk = $sdkMatch.Groups['version'].Value }

$lines = @(
    '# Release-candidate consumer report',
    '',
    "- SDK: $sdk",
    "- prefix: ``$(Split-Path -Leaf $prefixPath)`` (installed outside the source tree)",
    '',
    '## What ran',
    ''
)
foreach ($line in $reportLines) { $lines += "- $line" }

# --- Reproducibility gate -------------------------------------------------
# The repository root must not appear anywhere in the record.
$leaked = @($lines | Where-Object {
    $_.IndexOf($repositoryRoot, [System.StringComparison]::OrdinalIgnoreCase) -ge 0
})
if ($leaked.Count -gt 0) {
    foreach ($line in $leaked) { Write-Host "  LEAKED: $line" }
    throw ('The report quotes paths inside the repository. It is neither ' +
        'reproducible by a reader nor evidence that the package was consumed ' +
        'from an install.')
}

[System.IO.File]::WriteAllLines(
    [System.IO.Path]::GetFullPath($ReportFile),
    $lines,
    [System.Text.UTF8Encoding]::new($false))

if ($env:GITHUB_STEP_SUMMARY) {
    Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY -Value ($lines -join "`n")
}

Write-Host ''
Write-Host "Report written to '$ReportFile'; no repository paths in it."
