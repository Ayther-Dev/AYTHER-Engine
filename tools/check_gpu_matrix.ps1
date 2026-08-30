<#
.SYNOPSIS
    Runs the GPU test preset and refuses to call an omission a pass.

.DESCRIPTION
    The GPU oracles skip themselves when no Vulkan device answers. That is the
    right behaviour in the ordinary matrix, where the runners promise no
    hardware -- but inside the GPU job it is the failure mode the job exists to
    prevent. A run that skipped every test and exited 0 is indistinguishable, in
    a green tick, from a run that actually rendered eight frames and compared
    them.

    So this wrapper reports what ran, records the device and driver that
    answered, and FAILS when the suite was skipped rather than executed. An
    omission is reported as an omission.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Preset,

    # Prefixed to ctest, e.g. 'xvfb-run --auto-servernum' on a headless Linux
    # runner. Split on spaces.
    [string]$Launcher = '',

    [string]$ReportFile = 'gpu-report.md',

    # How many GPU tests must actually run. Zero would let an empty preset pass.
    [int]$MinimumTests = 1
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Verbose on purpose: the device line is logged by a PASSING test, and
# --output-on-failure would throw away exactly the record this wrapper
# exists to keep.
$arguments = @('--preset', $Preset, '-V')
$output = if ($Launcher) {
    $parts = $Launcher.Split(' ', [StringSplitOptions]::RemoveEmptyEntries)
    & $parts[0] @($parts[1..($parts.Length - 1)] + 'ctest' + $arguments) 2>&1
} else {
    & ctest @arguments 2>&1
}
$ctestExit = $LASTEXITCODE
$text = ($output | Out-String)
Write-Host $text

# --- What answered ---------------------------------------------------------
# vk_context logs the selected device on every context creation; the first one
# is the device the suite ran against.
$device = 'unknown'
$deviceMatch = [regex]::Match($text, 'GPU:\s*(?<line>[^\r\n]+)')
if ($deviceMatch.Success) { $device = $deviceMatch.Groups['line'].Value.Trim() }

# --- What ran --------------------------------------------------------------
$passed = ([regex]::Matches($text, '(?m)^\s*\d+/\d+ Test\s+#\d+:.*\s+Passed')).Count
$skipped = ([regex]::Matches($text, '(?m)^\s*\d+/\d+ Test\s+#\d+:.*\*\*\*Skipped')).Count
$failed = ([regex]::Matches($text, '(?m)^\s*\d+/\d+ Test\s+#\d+:.*\*\*\*(Failed|Exception)')).Count
$total = $passed + $skipped + $failed

# Wrapped so a single match, or none, still behaves like a collection under
# Set-StrictMode: a bare scalar has no .Count and would fault the report.
$skippedNames = @(
    [regex]::Matches($text, '(?m)^\s*\d+/\d+ Test\s+#\d+:\s*(?<name>\S+).*\*\*\*Skipped') |
        ForEach-Object { $_.Groups['name'].Value })

$lines = @(
    "# GPU matrix — $Preset",
    '',
    "- device/driver: ``$device``",
    "- tests run: $total  (passed $passed · skipped $skipped · failed $failed)",
    "- ctest exit code: $ctestExit"
)
if ($skippedNames.Count -gt 0) {
    $lines += ''
    $lines += '## Skipped'
    $lines += ''
    foreach ($name in $skippedNames) { $lines += "- $name" }
}
[System.IO.File]::WriteAllLines(
    [System.IO.Path]::GetFullPath($ReportFile),
    $lines,
    [System.Text.UTF8Encoding]::new($false))

# The report is the artifact; the summary is what a reviewer sees without
# downloading anything.
if ($env:GITHUB_STEP_SUMMARY) {
    Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY -Value ($lines -join "`n")
}

Write-Host ''
Write-Host "device/driver: $device"
Write-Host "run=$total passed=$passed skipped=$skipped failed=$failed"

# --- The gate --------------------------------------------------------------
if ($ctestExit -ne 0) {
    throw "GPU tests failed (ctest exit $ctestExit)."
}
if ($total -lt $MinimumTests) {
    throw ("The GPU preset registered $total tests; at least $MinimumTests were " +
        'expected. An empty suite exits 0 and would be reported as a pass.')
}
if ($skipped -gt 0) {
    throw ("$skipped GPU test(s) were SKIPPED: " + ($skippedNames -join ', ') +
        '. Inside the GPU job a skip is a failure -- the job exists to run ' +
        'them, and reporting a skip as green is the exact thing it prevents.')
}
if ($device -eq 'unknown') {
    throw ('No Vulkan device was reported. The suite cannot have exercised a ' +
        'GPU, so its result must not be recorded as one.')
}

Write-Host ''
Write-Host "GPU matrix verified for $Preset on $device."
