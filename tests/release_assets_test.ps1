[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$WorkDirectory,

    [Parameter(Mandatory = $true)]
    [string]$Finalizer,

    [Parameter(Mandatory = $true)]
    [string]$Packager
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$tag = 'v0.1.0-rc.5'
$root = [System.IO.Path]::GetFullPath($WorkDirectory)
$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$repositoryPrefix = $repositoryRoot.TrimEnd(
    [System.IO.Path]::DirectorySeparatorChar,
    [System.IO.Path]::AltDirectorySeparatorChar
) + [System.IO.Path]::DirectorySeparatorChar
if (-not $root.StartsWith(
        $repositoryPrefix,
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Release-asset test workspace must stay inside '$repositoryRoot'."
}
if (Test-Path -LiteralPath $root) {
    Remove-Item -LiteralPath $root -Recurse -Force
}
New-Item -ItemType Directory -Path $root | Out-Null

$stems = @(
    "ayther-engine-$tag-linux-x86_64",
    "ayther-engine-$tag-windows-x86_64",
    "ayther-engine-vpx-$tag-linux-x86_64",
    "ayther-engine-vpx-$tag-windows-x86_64"
)

foreach ($stem in $stems) {
    [System.IO.File]::WriteAllText(
        (Join-Path $root "$stem.zip"),
        "archive:$stem",
        [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllText(
        (Join-Path $root "$stem.spdx.json"),
        "{`"name`":`"$stem`"}",
        [System.Text.UTF8Encoding]::new($false))
}

& $Finalizer -Tag $tag -Directory $root

$manifest = Join-Path $root 'CHECKSUMS.sha256'
$lines = @(Get-Content -LiteralPath $manifest)
if ($lines.Count -ne 4) {
    throw "Expected four archive checksums, found $($lines.Count)."
}

$expectedNames = @($stems | ForEach-Object { "$_.zip" } | Sort-Object)
for ($index = 0; $index -lt $expectedNames.Count; $index++) {
    $name = $expectedNames[$index]
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $root $name)).Hash.ToLowerInvariant()
    if ($lines[$index] -cne "$hash *$name") {
        throw "Checksum line does not describe the final archive '$name'."
    }
}

$replaced = $false
try {
    & $Finalizer -Tag $tag -Directory $root
    $replaced = $true
}
catch {
    if ($_.Exception.Message -notlike 'Refusing to replace existing checksum manifest*') {
        throw
    }
}
if ($replaced) {
    throw 'The finalizer overwrote an immutable checksum manifest.'
}

Remove-Item -LiteralPath $manifest
[System.IO.File]::WriteAllText(
    (Join-Path $root "ayther-core-$tag-windows-x86_64.zip"),
    'unexpected core archive',
    [System.Text.UTF8Encoding]::new($false))

$acceptedUnexpected = $false
try {
    & $Finalizer -Tag $tag -Directory $root
    $acceptedUnexpected = $true
}
catch {
    if ($_.Exception.Message -notlike 'The release input set must contain exactly*') {
        throw
    }
}
if ($acceptedUnexpected) {
    throw 'The finalizer accepted an artifact outside the official four-ZIP set.'
}

$packageInput = Join-Path $root 'package-input'
New-Item -ItemType Directory -Path $packageInput | Out-Null
[System.IO.File]::WriteAllText(
    (Join-Path $packageInput 'payload.txt'),
    'deterministic payload',
    [System.Text.UTF8Encoding]::new($false))

$sourceEpoch = 1788378381
$archiveRoot = 'ayther-engine-clock-contract'
$packageA = Join-Path $root 'package-a/archive.zip'
$packageB = Join-Path $root 'package-b/archive.zip'
& $Packager `
    -InputDirectory $packageInput `
    -OutputFile $packageA `
    -ArchiveRoot $archiveRoot `
    -SourceDateEpoch $sourceEpoch
& $Packager `
    -InputDirectory $packageInput `
    -OutputFile $packageB `
    -ArchiveRoot $archiveRoot `
    -SourceDateEpoch $sourceEpoch

$hashA = (Get-FileHash -Algorithm SHA256 -LiteralPath $packageA).Hash
$hashB = (Get-FileHash -Algorithm SHA256 -LiteralPath $packageB).Hash
if ($hashA -cne $hashB) {
    throw 'Packaging the same install tree twice was not byte-identical.'
}

$unpacked = Join-Path $root 'package-unpacked'
Expand-Archive -LiteralPath $packageA -DestinationPath $unpacked
$payload = Get-Item -LiteralPath (Join-Path $unpacked "$archiveRoot/payload.txt")
$sourceTimestamp = [DateTimeOffset]::FromUnixTimeSeconds($sourceEpoch)
if ($payload.LastWriteTimeUtc -gt $sourceTimestamp.UtcDateTime) {
    throw 'Extracted release content has a future timestamp and can loop CMake regeneration.'
}

$overwroteArchive = $false
try {
    & $Packager `
        -InputDirectory $packageInput `
        -OutputFile $packageA `
        -ArchiveRoot $archiveRoot `
        -SourceDateEpoch $sourceEpoch
    $overwroteArchive = $true
}
catch {
    if ($_.Exception.Message -notlike 'Refusing to overwrite existing archive*') {
        throw
    }
}
if ($overwroteArchive) {
    throw 'The packager replaced an existing archive under the same name.'
}

Write-Host 'Release asset finalization contract passed.'
