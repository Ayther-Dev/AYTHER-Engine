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
$optionalCoreTests = @('audio_mute', 'audio_output', 'render_output', 'subsystem_routing')
$requiredCoreTests = @(
    'abi_negociacion', 'abi_frame_delta', 'abi_multilayer',
    'abi_lecturas', 'abi_escrituras', 'e2e_determinismo'
)
$testNames = $optionalCoreTests + $requiredCoreTests

function Write-TestLog([string]$Status = 'Passed', [hashtable]$Overrides = @{},
                       [string[]]$AdditionalTests = @()) {
    $names = $testNames + $AdditionalTests
    $lines = for ($index = 0; $index -lt $names.Count; ++$index) {
        $name = $names[$index]
        $resultStatus = if ($Overrides.ContainsKey($name)) { $Overrides[$name] } else { $Status }
        "$($index + 1)/$($names.Count) Test #$($index + 1): $name ........ $resultStatus 0.01 sec"
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

try {
    Set-Content -LiteralPath $lockPath -Value 'vram_file = renamed-core.bin # pinned'
    $optionalSkips = @{}
    foreach ($testName in $optionalCoreTests) { $optionalSkips[$testName] = '***Skipped' }
    Write-TestLog -Overrides $optionalSkips
    Assert-Checker $true 'A missing optional core explains skips'
    Write-TestLog -Overrides @{ abi_negociacion = '***Skipped' }
    Assert-Checker $false 'A missing optional core cannot excuse a bundled-core test skip'
    Write-TestLog -Overrides @{ future_test = '***Skipped' } -AdditionalTests 'future_test'
    Assert-Checker $false 'A missing optional core cannot excuse an unrelated test skip'
    Set-Content -LiteralPath $corePath -Value 'fixture'
    Write-TestLog -Overrides $optionalSkips
    Assert-Checker $false 'An available core cannot produce skipped tests'
    Write-TestLog 'Passed'
    Assert-Checker $true 'All ten passing results are accepted'
    Assert-Checker $true 'A valid explicit override is accepted' $corePath
    Assert-Checker $false 'An invalid explicit override never falls back' (Join-Path $testDirectory 'missing.bin')
    foreach ($testName in $requiredCoreTests) {
        Write-TestLog -Overrides @{ $testName = '***Skipped' }
        Assert-Checker $false "A skipped $testName cannot claim coverage"
    }
    Write-TestLog -Overrides @{ future_test = '***Skipped' } -AdditionalTests 'future_test'
    Assert-Checker $false 'A skipped additional test is rejected'
    Write-TestLog -Overrides @{ future_test = '***Failed' } -AdditionalTests 'future_test'
    Assert-Checker $false 'A failed additional test is rejected'
    Write-TestLog -AdditionalTests 'future_test'
    Assert-Checker $true 'A passing additional test is accepted'
    Write-TestLog -Overrides @{ abi_lecturas = '***Failed' }
    Assert-Checker $false 'An ABI failure remains a failure'
    Write-TestLog -Overrides @{ abi_lecturas = '***Not Run' }
    Assert-Checker $false 'An ABI test that did not run is rejected'
    Write-TestLog -AdditionalTests 'abi_negociacion'
    Assert-Checker $false 'Duplicate results are rejected'
    Write-TestLog
    $incompleteLog = Get-Content -LiteralPath $logPath | Where-Object { $_ -notmatch ': abi_negociacion ' }
    Set-Content -LiteralPath $logPath -Value $incompleteLog
    Assert-Checker $false 'A missing ABI result is rejected'
    Write-TestLog '***Failed'
    Assert-Checker $false 'Failures remain failures'
    Set-Content -LiteralPath $logPath -Value '100% tests passed, 0 tests failed out of 0'
    Assert-Checker $false 'An empty suite cannot claim coverage'
    Write-TestLog 'Passed'
    Set-Content -LiteralPath $lockPath -Value 'tag = missing-filename'
    Assert-Checker $false 'Malformed lock metadata is rejected'
} finally {
    # Every generated file stays below this unique directory within WorkDirectory.
    $resolvedRoot = [IO.Path]::GetFullPath($WorkDirectory).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $resolvedDirectory = [IO.Path]::GetFullPath($testDirectory)
    if (-not $resolvedDirectory.StartsWith($resolvedRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Test cleanup directory escaped the work directory.'
    }
    Remove-Item -LiteralPath $resolvedDirectory -Recurse -Force
}
