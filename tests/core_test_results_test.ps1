[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Checker,
    [Parameter(Mandatory)][string]$WorkDirectory
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$testDirectory = Join-Path $WorkDirectory ([guid]::NewGuid().ToString('N'))
$coreDirectory = Join-Path $testDirectory 'third_party/cores'
New-Item -ItemType Directory -Path $coreDirectory -Force | Out-Null
$logPath = Join-Path $testDirectory 'ctest.log'
$lockPath = Join-Path $coreDirectory 'core.lock'
$corePath = Join-Path $coreDirectory 'renamed-core.bin'
$pwsh = (Get-Process -Id $PID).Path
$testNames = @('audio_mute', 'audio_output', 'render_output', 'subsystem_routing')

function Write-TestLog([string]$Status) {
    $lines = for ($index = 0; $index -lt $testNames.Count; ++$index) {
        "$($index + 1)/4 Test #$($index + 1): $($testNames[$index]) ........ $Status 0.01 sec"
    }
    Set-Content -LiteralPath $logPath -Value $lines
}
function Assert-Checker([bool]$ShouldPass, [string]$Description, [string]$Override = '') {
    $output = & $pwsh -NoProfile -File $Checker -LogPath $logPath `
        -SourceDirectory $testDirectory -CorePath $Override 2>&1
    if (($LASTEXITCODE -eq 0) -ne $ShouldPass) {
        throw "$Description failed: $($output -join [Environment]::NewLine)"
    }
    Write-Output "PASS: $Description"
}

Set-Content -LiteralPath $lockPath -Value 'vram_file = renamed-core.bin # pinned'
Write-TestLog '***Skipped'
Assert-Checker $true 'A missing optional core explains skips'
Set-Content -LiteralPath $corePath -Value 'fixture'
Assert-Checker $false 'An available core cannot produce skipped tests'
Write-TestLog 'Passed'
Assert-Checker $true 'All four passing results are accepted'
Assert-Checker $false 'An invalid explicit override never falls back' (Join-Path $testDirectory 'missing.bin')
Write-TestLog '***Failed'
Assert-Checker $false 'Failures remain failures'
Set-Content -LiteralPath $logPath -Value '100% tests passed, 0 tests failed out of 0'
Assert-Checker $false 'An empty suite cannot claim coverage'
Write-TestLog 'Passed'
Set-Content -LiteralPath $lockPath -Value 'tag = missing-filename'
Assert-Checker $false 'Malformed lock metadata is rejected'

# Every generated file stays below this unique directory within WorkDirectory.
$resolvedRoot = [IO.Path]::GetFullPath($WorkDirectory).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
$resolvedDirectory = [IO.Path]::GetFullPath($testDirectory)
if (-not $resolvedDirectory.StartsWith($resolvedRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Test cleanup directory escaped the work directory.'
}
Remove-Item -LiteralPath $resolvedDirectory -Recurse -Force
