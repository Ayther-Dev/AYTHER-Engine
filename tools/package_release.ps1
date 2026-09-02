[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$InputDirectory,

    [Parameter(Mandatory = $true)]
    [string]$OutputFile,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9._-]+$')]
    [string]$ArchiveRoot,

    [Parameter(Mandatory = $true)]
    [ValidateRange(315532800, [long]::MaxValue)]
    [long]$SourceDateEpoch
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$inputPath = (Resolve-Path -LiteralPath $InputDirectory).Path
$outputPath = [System.IO.Path]::GetFullPath($OutputFile)
$inputPrefix = $inputPath.TrimEnd(
    [System.IO.Path]::DirectorySeparatorChar,
    [System.IO.Path]::AltDirectorySeparatorChar
) + [System.IO.Path]::DirectorySeparatorChar
if ($outputPath.StartsWith($inputPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Release output must be outside the input tree '$inputPath'."
}
$outputDirectory = Split-Path -Parent $outputPath
if (-not (Test-Path -LiteralPath $outputDirectory)) {
    New-Item -ItemType Directory -Path $outputDirectory | Out-Null
}
if (Test-Path -LiteralPath $outputPath) {
    throw "Refusing to overwrite existing archive '$outputPath'."
}

$files = [System.Collections.Generic.List[string]]::new()
Get-ChildItem -LiteralPath $inputPath -Recurse -File | ForEach-Object {
    $relative = [System.IO.Path]::GetRelativePath($inputPath, $_.FullName).Replace('\', '/')
    $files.Add($relative)
}
$files.Sort([System.StringComparer]::Ordinal)
if ($files.Count -eq 0) {
    throw "Release input '$inputPath' contains no files."
}

# ZIP stores a timezone-free DOS wall clock. Writing the exact UTC commit time
# makes a freshly extracted package appear several hours in the future for
# consumers west of UTC; Ninja then reruns CMake forever because installed
# AytherConfig.cmake is newer than build.ninja. Use midnight on the previous
# UTC day, still deterministically derived from SOURCE_DATE_EPOCH and safely in
# the past in every real-world timezone.
$sourceTimestamp = [DateTimeOffset]::FromUnixTimeSeconds($SourceDateEpoch)
$safeDate = [DateTime]::SpecifyKind(
    $sourceTimestamp.UtcDateTime.Date.AddDays(-1),
    [DateTimeKind]::Unspecified)
$timestamp = [DateTimeOffset]::new($safeDate, [TimeSpan]::Zero)
$zipEpoch = [DateTimeOffset]::new(
    [DateTime]::new(1980, 1, 1),
    [TimeSpan]::Zero)
if ($timestamp -lt $zipEpoch) {
    $timestamp = $zipEpoch
}
$stream = [System.IO.File]::Open($outputPath, [System.IO.FileMode]::CreateNew)
try {
    $archive = [System.IO.Compression.ZipArchive]::new(
        $stream,
        [System.IO.Compression.ZipArchiveMode]::Create,
        $false
    )
    try {
        foreach ($relative in $files) {
            $entry = $archive.CreateEntry(
                "$ArchiveRoot/$relative",
                [System.IO.Compression.CompressionLevel]::Optimal
            )
            $entry.LastWriteTime = $timestamp
            $entryStream = $entry.Open()
            try {
                $source = [System.IO.File]::OpenRead((Join-Path $inputPath $relative))
                try {
                    $source.CopyTo($entryStream)
                }
                finally {
                    $source.Dispose()
                }
            }
            finally {
                $entryStream.Dispose()
            }
        }
    }
    finally {
        $archive.Dispose()
    }
}
finally {
    $stream.Dispose()
}

$digest = (Get-FileHash -Algorithm SHA256 -LiteralPath $outputPath).Hash.ToLowerInvariant()
Write-Host "$digest  $([System.IO.Path]::GetFileName($outputPath))"
