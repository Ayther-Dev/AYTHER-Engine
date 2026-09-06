<#
.SYNOPSIS
Checks native CTest coverage and rejects unexplained skipped tests.
.DESCRIPTION
Consumes a full native CTest log and requires all ten core-related test results.
The ABI and determinism tests use bundled cores and must run. Only the four
external-core tests may skip when the optional binary named by core.lock is
absent. A configured core override must exist. Failures, duplicate results,
missing required results, and skips in any other test produce a nonzero exit.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$LogPath,
    [string]$SourceDirectory = '.',
    [string]$CorePath = $env:AYTHER_ABI_CORE
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

try {
    $log = Get-Content -LiteralPath $LogPath -Raw
    $optionalCoreTests = @('audio_mute', 'audio_output', 'render_output', 'subsystem_routing')
    $requiredCoreTests = @(
        'abi_negociacion', 'abi_frame_delta', 'abi_multilayer',
        'abi_lecturas', 'abi_escrituras', 'e2e_determinismo'
    )
    $expectedTests = $optionalCoreTests + $requiredCoreTests
    $pattern = '(?m)^\s*\d+/\d+ Test\s+#\d+:\s*(?<name>\S+)\s+\.+\s*' +
        '(?<status>Passed|\*\*\*Skipped|\*\*\*[^\r\n]+)'
    $results = [regex]::Matches($log, $pattern)
    $seenTests = @{}
    $skippedTests = @()
    foreach ($result in $results) {
        $testName = $result.Groups['name'].Value
        if ($seenTests.ContainsKey($testName)) { throw "Duplicate result for $testName." }
        $seenTests[$testName] = $true
        $status = $result.Groups['status'].Value
        if ($status -eq '***Skipped') { $skippedTests += $testName }
        elseif ($status -ne 'Passed') { throw "CTest test failed: $testName ($status)." }
    }
    foreach ($testName in $expectedTests) {
        if (-not $seenTests.ContainsKey($testName)) { throw "Missing result for $testName." }
    }
    $unexpectedSkips = @($skippedTests | Where-Object { $_ -notin $optionalCoreTests })
    if ($unexpectedSkips.Count -gt 0) {
        throw "These tests must run regardless of optional core availability: $($unexpectedSkips -join ', ')."
    }

    if ($CorePath) {
        if (-not (Test-Path -LiteralPath $CorePath -PathType Leaf)) {
            throw "AYTHER_ABI_CORE is not a core file: $CorePath"
        }
        $coreAvailable = $true
    } else {
        $lockPath = Join-Path $SourceDirectory 'third_party/cores/core.lock'
        $entries = @(
            Get-Content -LiteralPath $lockPath | ForEach-Object {
                $content = ($_ -split '#', 2)[0]
                if ($content -match '^\s*vram_file\s*=\s*(.*?)\s*$') { $Matches[1] }
            }
        )
        if ($entries.Count -ne 1 -or -not $entries[0] -or
            $entries[0] -match '[/\\]' -or $entries[0] -in @('.', '..')) {
            throw "Expected one vram_file filename in $lockPath."
        }
        $CorePath = Join-Path (Split-Path $lockPath) $entries[0]
        $coreAvailable = Test-Path -LiteralPath $CorePath -PathType Leaf
        if (-not $coreAvailable -and (Test-Path -LiteralPath $CorePath)) {
            throw "Locked core is not a file: $CorePath"
        }
    }

    if ($skippedTests.Count -eq 0) {
        Write-Output 'All ten core-related tests passed; no CTest results were skipped.'
    } elseif ($coreAvailable) {
        throw "Core exists at $CorePath but these tests were skipped: $($skippedTests -join ', ')."
    } else {
        Write-Warning "Optional core absent at $CorePath. Not exercised: $($skippedTests -join ', ')."
    }
    exit 0
} catch {
    Write-Error -ErrorAction Continue $_
    exit 1
}
