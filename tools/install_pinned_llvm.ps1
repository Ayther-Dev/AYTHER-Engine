#requires -Version 7
<#
.SYNOPSIS
Installs the LLVM that the Windows presets compile with (clang-cl under
$env:ProgramFiles\LLVM) at a pinned version and verifies it.

.DESCRIPTION
The windows-2025-vs2026 image moves MSVC and LLVM independently. Image
20261116 ships MSVC 14.52, whose STL refuses Clang older than 22
(STL1000), next to LLVM 20, so a job that lands on it cannot compile any
C++ with the presets. Pinning LLVM here keeps the compiler stable across
image rollouts; raise the pin together with the local toolchain.
#>
[CmdletBinding()]
param(
    [string]$Version = '22.1.8'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not $IsWindows) { throw 'install_pinned_llvm.ps1 only runs on Windows.' }

# The presets resolve $env{ProgramFiles} in the 64-bit CMake process; a 32-bit
# shell sees "Program Files (x86)" there, so prefer the 64-bit location.
$programFiles = if ($env:ProgramW6432) { $env:ProgramW6432 } else { $env:ProgramFiles }
$clang = Join-Path $programFiles 'LLVM/bin/clang-cl.exe'

function Get-ClangVersion {
    if (-not (Test-Path -LiteralPath $clang)) { return $null }
    $line = (& $clang --version | Select-Object -First 1)
    if ($line -match 'clang version (\d+\.\d+\.\d+)') { return $Matches[1] }
    return $null
}

$current = Get-ClangVersion
if ($current -eq $Version) {
    Write-Host "LLVM $Version already installed."
} else {
    Write-Host "Installing LLVM $Version (found: $(if ($current) { $current } else { 'none' }))."
    choco install llvm --version=$Version --yes --no-progress --force --allow-downgrade
    if ($LASTEXITCODE -ne 0) { throw "choco install llvm $Version failed ($LASTEXITCODE)." }
    $current = Get-ClangVersion
}

if ($current -ne $Version) {
    throw "clang-cl at $clang reports $current, expected $Version."
}
$major = $Version.Split('.')[0]
if (-not (Test-Path -LiteralPath (Join-Path $programFiles "LLVM/lib/clang/$major/include"))) {
    throw "The LLVM $Version resource headers are missing under lib/clang/$major."
}
Write-Host "clang-cl $current at $clang."
