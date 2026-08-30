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

# A pre-release tag carries a suffix the numeric surfaces cannot express:
# CMake's project(VERSION) and the AYTHER_VERSION_* macros are MAJOR.MINOR.PATCH
# and nothing else. So the CORE version is what every surface must agree on,
# while Cargo and vcpkg -- both of which do understand SemVer pre-release -- may
# additionally carry the full tag.
$full = $Tag.Substring(1)
$core = ($full -split '-', 2)[0]
$prerelease = if ($full -eq $core) { '' } else { $full.Substring($core.Length + 1) }

$strict = [ordered]@{
    CMake = $cmakeMatch.Groups['version'].Value
    Header = '{0}.{1}.{2}' -f $headerMajor.Groups['version'].Value,
        $headerMinor.Groups['version'].Value,
        $headerPatch.Groups['version'].Value
}
$semver = [ordered]@{
    Cargo = $cargoMatch.Groups['version'].Value
    vcpkg = [string]$vcpkg.'version-semver'
}

foreach ($surface in $strict.GetEnumerator()) {
    if ($surface.Value -ne $core) {
        throw "Release version mismatch: tag core=$core, $($surface.Key)=$($surface.Value)."
    }
}

foreach ($surface in $semver.GetEnumerator()) {
    if ($surface.Value -ne $core -and $surface.Value -ne $full) {
        throw ("Release version mismatch: tag=$full (core $core), " +
            "$($surface.Key)=$($surface.Value).")
    }
}

if ($prerelease) {
    Write-Host "Release version contract verified for $Tag (pre-release '$prerelease' of $core)."
} else {
    Write-Host "Release version contract verified for $Tag."
}
