[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^v\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$')]
    [string]$Tag,

    [string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = (Resolve-Path -LiteralPath $RepositoryRoot).Path
$cargo = Get-Content -Raw -LiteralPath (Join-Path $root 'Cargo.toml')
$cmake = Get-Content -Raw -LiteralPath (Join-Path $root 'CMakeLists.txt')
$vcpkg = Get-Content -Raw -LiteralPath (Join-Path $root 'vcpkg.json') | ConvertFrom-Json
$header = Get-Content -Raw -LiteralPath (Join-Path $root 'include/ayther/ayther_version.h')

$cargoMatch = [regex]::Match($cargo, '(?m)^version\s*=\s*"(?<version>[^"]+)"\s*$')
$cmakeMatch = [regex]::Match($cmake, 'project\s*\(\s*AYTHER\s+VERSION\s+(?<version>\d+\.\d+\.\d+)', 'IgnoreCase')
$headerMajor = [regex]::Match($header, '(?m)^#define\s+AYTHER_VERSION_MAJOR\s+(?<version>\d+)\s*$')
$headerMinor = [regex]::Match($header, '(?m)^#define\s+AYTHER_VERSION_MINOR\s+(?<version>\d+)\s*$')
$headerPatch = [regex]::Match($header, '(?m)^#define\s+AYTHER_VERSION_PATCH\s+(?<version>\d+)\s*$')

if (-not $cargoMatch.Success -or -not $cmakeMatch.Success -or
    -not $headerMajor.Success -or -not $headerMinor.Success -or -not $headerPatch.Success) {
    throw 'Could not read every release-version surface.'
}

$expected = $Tag.Substring(1)
$versions = [ordered]@{
    Cargo = $cargoMatch.Groups['version'].Value
    CMake = $cmakeMatch.Groups['version'].Value
    vcpkg = [string]$vcpkg.'version-semver'
    Header = '{0}.{1}.{2}' -f $headerMajor.Groups['version'].Value,
        $headerMinor.Groups['version'].Value,
        $headerPatch.Groups['version'].Value
}

foreach ($surface in $versions.GetEnumerator()) {
    if ($surface.Value -ne $expected) {
        throw "Release version mismatch: tag=$expected, $($surface.Key)=$($surface.Value)."
    }
}

Write-Host "Release version contract verified for $Tag."
