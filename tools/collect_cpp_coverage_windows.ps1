[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$BuildDirectory,
    [Parameter(Mandatory)] [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
$build = [IO.Path]::GetFullPath($BuildDirectory)
$output = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $output | Out-Null
$profiles = @(Get-ChildItem -LiteralPath (Join-Path $build 'coverage-profiles') -Filter '*.profraw' -File)
if ($profiles.Count -eq 0) { throw 'COVERAGE_PROFILES_MISSING: native tests produced no profiles.' }
$profileData = Join-Path $output 'native.profdata'
& llvm-profdata merge -sparse @($profiles | ForEach-Object FullName) -o $profileData
if ($LASTEXITCODE -ne 0) { throw 'LLVM_PROFDATA_FAILED: native profiles could not be merged.' }
$names = @(
    'ayther_tessera_session_test.exe',
    'ayther_tessera_pack_test.exe',
    'ayther_tessera_presentation_test.exe',
    'ayther_tessera_controls_test.exe'
)
$binaries = @($names | ForEach-Object { Join-Path (Join-Path $build 'bin') $_ })
foreach ($binary in $binaries) {
    if (-not (Test-Path -LiteralPath $binary -PathType Leaf)) {
        throw "COVERAGE_BINARY_MISSING: $binary"
    }
}
$arguments = @('export', $binaries[0])
foreach ($binary in $binaries[1..($binaries.Count - 1)]) {
    $arguments += @('--object', $binary)
}
$arguments += @(
    "--instr-profile=$profileData",
    '--format=lcov',
    '--ignore-filename-regex=(^|[/\\])(third_party|vcpkg_installed|_deps|CMakeFiles)[/\\]'
)
$report = Join-Path $output 'native.lcov'
& llvm-cov @arguments | Out-File -LiteralPath $report -Encoding utf8NoBOM
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $report -PathType Leaf)) {
    throw 'LLVM_COV_FAILED: native LCOV export failed.'
}
Write-Output "Native C++ coverage: $($profiles.Count) profiles; $($binaries.Count) binaries; $report"
