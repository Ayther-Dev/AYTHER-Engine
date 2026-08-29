# set_license.ps1 — apply an already-reviewed Engine license text.
#
# Repository separation is the product boundary. This tool updates only the
# Engine repository and refuses to create or edit sibling product licenses.
# Legal policy must name the requested SPDX identifier before any write occurs,
# preventing a partial mechanical change from silently contradicting prose.
#
# Usage:
#   pwsh tools/set_license.ps1 -Id MPL-2.0 -TextFile C:\path\MPL-2.0.txt

param(
    [Parameter(Mandatory)][string]$Id,
    [Parameter(Mandatory)][string]$TextFile,
    [string]$Holder = "David Lazarte",
    [string]$Year = "2026"
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path "$PSScriptRoot/..").Path

if (-not (Test-Path -LiteralPath $TextFile)) {
    throw "License text does not exist: $TextFile"
}

$text = Get-Content -LiteralPath $TextFile -Raw
if ($text.Trim().Length -lt 200) {
    throw "License text appears truncated ($($text.Length) characters)"
}

$legalPath = Join-Path $repo "docs/LEGAL_AND_DISTRIBUTION.md"
$legal = Get-Content -LiteralPath $legalPath -Raw
if ($legal -notmatch [regex]::Escape($Id)) {
    throw "LEGAL_AND_DISTRIBUTION.md does not name $Id. Review legal policy before applying a new license."
}

$text = $text -replace '\[yyyy\]|<year>|\{yyyy\}', $Year
$text = $text -replace '\[name of copyright owner\]|<copyright holders>|\{name of copyright owner\}', $Holder

$licensePath = Join-Path $repo "LICENSE"
[System.IO.File]::WriteAllText($licensePath, $text)
Write-Host "  LICENSE    -> $Id"

$cargoPath = Join-Path $repo "Cargo.toml"
$cargo = Get-Content -LiteralPath $cargoPath -Raw
if ($cargo -match '(?m)^license\s*=') {
    $cargo = $cargo -replace '(?m)^license\s*=.*$', "license = `"$Id`""
} else {
    $cargo = $cargo -replace '(?m)^(repository\s*=.*)$', "`$1`nlicense = `"$Id`""
}
[System.IO.File]::WriteAllText($cargoPath, $cargo)
Write-Host "  Cargo.toml -> license = `"$Id`""

Write-Host "Verify with: cmake -DAYTHER_REPO=$repo -P tools/check_license.cmake"
