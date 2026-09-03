<#
.SYNOPSIS
    Rejects producer/monorepo paths in external-consumer CMake evidence.

.DESCRIPTION
    Scans CMakeCache.txt files and the imported-target property report emitted
    by tests/external_package_consumer. Paths are normalized for slash style,
    repeated separators, and case so a Windows spelling cannot evade the gate.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string[]]$CacheFiles,

    [Parameter(Mandatory = $true)]
    [string]$ImportedTargetsReport,

    [Parameter(Mandatory = $true)]
    [string[]]$ForbiddenRoots
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function ConvertTo-AuditText {
    param([Parameter(Mandatory = $true)][string]$Value)

    return (($Value.Replace('\', '/').ToLowerInvariant()) -replace '/+', '/')
}

$evidenceFiles = @($CacheFiles) + @($ImportedTargetsReport)
foreach ($evidenceFile in $evidenceFiles) {
    if (-not (Test-Path -LiteralPath $evidenceFile -PathType Leaf)) {
        throw "External-consumer path evidence is missing: '$evidenceFile'."
    }
}

$forbidden = @(foreach ($forbiddenRoot in $ForbiddenRoots) {
    if ([string]::IsNullOrWhiteSpace($forbiddenRoot)) {
        throw 'A forbidden root cannot be empty.'
    }
    ConvertTo-AuditText ([System.IO.Path]::GetFullPath($forbiddenRoot).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar))
})

$violations = [System.Collections.Generic.List[string]]::new()
foreach ($evidenceFile in $evidenceFiles) {
    $resolvedEvidence = (Resolve-Path -LiteralPath $evidenceFile).Path
    $contents = ConvertTo-AuditText (
        [System.IO.File]::ReadAllText($resolvedEvidence))
    foreach ($forbiddenRoot in $forbidden) {
        if ($contents.Contains($forbiddenRoot, [System.StringComparison]::Ordinal)) {
            $violations.Add("$resolvedEvidence contains forbidden root '$forbiddenRoot'")
        }
    }
}

if ($violations.Count -ne 0) {
    foreach ($violation in $violations) {
        Write-Host "  LEAK  $violation"
    }
    throw 'External consumer retained a producer checkout, build, or monorepo path.'
}

$cacheCount = @($CacheFiles).Count
$forbiddenCount = $forbidden.Count
Write-Host ((
    "External path audit passed: {0} cache file(s), one imported-target report, " +
    "and {1} forbidden root(s).") -f $cacheCount, $forbiddenCount)
