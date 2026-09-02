<#
.SYNOPSIS
    Validates the immutable Engine release input set and writes its checksums.

.DESCRIPTION
    A release candidate has exactly two Engine variants for each supported
    platform, plus one SPDX document per archive. The checksum manifest is
    created only after all four final ZIP files are present and is never
    overwritten. This keeps a tag/name pair bound to one byte sequence.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^v\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$')]
    [string]$Tag,

    [Parameter(Mandatory = $true)]
    [string]$Directory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = (Resolve-Path -LiteralPath $Directory).Path
$checksumPath = Join-Path $root 'CHECKSUMS.sha256'
if (Test-Path -LiteralPath $checksumPath) {
    throw "Refusing to replace existing checksum manifest '$checksumPath'."
}

$expected = [System.Collections.Generic.List[string]]::new()
foreach ($product in 'ayther-engine', 'ayther-engine-vpx') {
    foreach ($platform in 'linux-x86_64', 'windows-x86_64') {
        $stem = "$product-$Tag-$platform"
        $expected.Add("$stem.zip")
        $expected.Add("$stem.spdx.json")
    }
}

$expected.Sort([System.StringComparer]::Ordinal)
$actual = [System.Collections.Generic.List[string]]::new()
Get-ChildItem -LiteralPath $root -File | ForEach-Object {
    $actual.Add($_.Name)
}
$actual.Sort([System.StringComparer]::Ordinal)

$missing = @($expected | Where-Object { $_ -cnotin $actual })
$unexpected = @($actual | Where-Object { $_ -cnotin $expected })
foreach ($name in $missing) { Write-Host "  MISSING     $name" }
foreach ($name in $unexpected) { Write-Host "  UNEXPECTED  $name" }
if ($missing.Count -gt 0 -or $unexpected.Count -gt 0) {
    throw 'The release input set must contain exactly four Engine ZIPs and their four SPDX documents.'
}

$archiveNames = @($expected | Where-Object { $_.EndsWith('.zip') })
$lines = foreach ($archiveName in $archiveNames) {
    $archivePath = Join-Path $root $archiveName
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $archivePath).Hash.ToLowerInvariant()
    "$hash *$archiveName"
}

$stream = [System.IO.File]::Open(
    $checksumPath,
    [System.IO.FileMode]::CreateNew,
    [System.IO.FileAccess]::Write,
    [System.IO.FileShare]::None)
try {
    $writer = [System.IO.StreamWriter]::new(
        $stream,
        [System.Text.UTF8Encoding]::new($false))
    try {
        foreach ($line in $lines) {
            $writer.WriteLine($line)
        }
    }
    finally {
        $writer.Dispose()
    }
}
finally {
    $stream.Dispose()
}

Write-Host "Release input set verified: 4 archives and 4 SPDX documents."
Write-Host "Wrote final archive digests to '$checksumPath'."
