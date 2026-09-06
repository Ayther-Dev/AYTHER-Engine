<#
.SYNOPSIS
Checks that the four external-core oracles ran or have a verifiable missing core.
.DESCRIPTION
Consumes a full native CTest log. A configured core override must exist. Without
an override, only absence of the binary named by core.lock permits skipped tests.
Malformed logs, missing test results, and failures always produce a nonzero exit.
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
    $expectedTests = @('audio_mute', 'audio_output', 'render_output', 'subsystem_routing')
    $skippedTests = @()
    foreach ($testName in $expectedTests) {
        $pattern = '(?m)^\s*\d+/\d+ Test\s+#\d+:\s*' +
            [regex]::Escape($testName) + '\s+\.+\s*(?<status>Passed|\*\*\*Skipped|\*\*\*[^\r\n]+)'
        $results = [regex]::Matches($log, $pattern)
        if ($results.Count -ne 1) { throw "Expected one result for $testName, found $($results.Count)." }
        $status = $results[0].Groups['status'].Value
        if ($status -eq '***Skipped') { $skippedTests += $testName }
        elseif ($status -ne 'Passed') { throw "Core test failed: $testName ($status)." }
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
        Write-Output 'All four external-core oracles passed.'
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
