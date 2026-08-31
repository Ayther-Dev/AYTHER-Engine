# Runs an installed-package consumer with a hard link-only deadline and checks
# that its report does not disclose the producer checkout.
param(
    [Parameter(Mandatory = $true)]
    [string]$Executable,
    [int]$TimeoutSeconds = 5,
    [string]$ForbiddenPath = ""
)

$ErrorActionPreference = "Stop"
$resolvedExecutable = (Resolve-Path -LiteralPath $Executable).Path
$stdoutPath = [System.IO.Path]::GetTempFileName()
$stderrPath = [System.IO.Path]::GetTempFileName()
$process = $null

try {
    $startArguments = @{
        FilePath = $resolvedExecutable
        PassThru = $true
        RedirectStandardOutput = $stdoutPath
        RedirectStandardError = $stderrPath
    }
    if ($IsWindows) {
        $startArguments.WindowStyle = 'Hidden'
    }
    else {
        $startArguments.NoNewWindow = $true
    }
    $process = Start-Process @startArguments

    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        Stop-Process -Id $process.Id -Force
        throw "package consumer exceeded the $TimeoutSeconds-second link-only deadline"
    }
    # WaitForExit() without a timeout drains redirected asynchronous streams.
    $process.WaitForExit()
    $stdout = Get-Content -LiteralPath $stdoutPath -Raw -ErrorAction SilentlyContinue
    $stderr = Get-Content -LiteralPath $stderrPath -Raw -ErrorAction SilentlyContinue
    if ($stdout) { Write-Host $stdout.TrimEnd() }
    if ($stderr) { Write-Error $stderr.TrimEnd() }
    if ($process.ExitCode -ne 0) {
        throw "package consumer exited with code $($process.ExitCode)"
    }

    if ($ForbiddenPath) {
        $normalizedForbidden = [System.IO.Path]::GetFullPath($ForbiddenPath)
        $report = "$stdout`n$stderr"
        if ($report.Contains($normalizedForbidden,
                [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "package consumer report leaked the producer checkout path"
        }
    }
    Write-Host "  [ OK ] link-only consumer completed within $TimeoutSeconds seconds"
}
finally {
    if ($process -and -not $process.HasExited) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    }
    Remove-Item -LiteralPath $stdoutPath, $stderrPath -Force `
        -ErrorAction SilentlyContinue
}
