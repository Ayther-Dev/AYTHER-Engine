<#
.SYNOPSIS
    Verifies and extracts one attested release candidate from CI artifact storage.

.DESCRIPTION
    This command is for the clean external-consumer jobs that run before GitHub
    Release publication. It verifies the final SHA-256 manifest, the keyless
    Sigstore bundle, and GitHub SLSA provenance before extracting or inspecting
    the package payload. The only pipeline output is the extracted CMake prefix.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$CandidateDirectory,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^v\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$')]
    [string]$Tag,

    [Parameter(Mandatory = $true)]
    [ValidateSet('ayther-engine', 'ayther-engine-vpx')]
    [string]$Product,

    [Parameter(Mandatory = $true)]
    [ValidateSet('linux-x86_64', 'windows-x86_64')]
    [string]$Platform,

    [Parameter(Mandatory = $true)]
    [string]$DestinationDirectory,

    [Parameter(Mandatory = $true)]
    [string]$PayloadChecker,

    [string]$Repository = 'Ayther-Dev/AYTHER-Engine'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

foreach ($tool in 'cosign', 'gh') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "Required verifier '$tool' is not on PATH."
    }
}

$candidateRoot = (Resolve-Path -LiteralPath $CandidateDirectory).Path
$payloadCheckerPath = (Resolve-Path -LiteralPath $PayloadChecker).Path
$name = "$Product-$Tag-$Platform"
$archiveName = "$name.zip"
$archivePath = Join-Path $candidateRoot $archiveName
$checksumPath = Join-Path $candidateRoot 'CHECKSUMS.sha256'
$bundlePath = "$archivePath.sigstore.json"
foreach ($requiredFile in $archivePath, $checksumPath, $bundlePath) {
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw "Release-candidate verification input is missing: '$requiredFile'."
    }
}

$escapedArchiveName = [regex]::Escape($archiveName)
$checksumLines = @(Get-Content -LiteralPath $checksumPath | Where-Object {
    $_ -match "^(?<hash>[0-9a-fA-F]{64}) [ *]$escapedArchiveName$"
})
if ($checksumLines.Count -ne 1) {
    throw "CHECKSUMS.sha256 must contain exactly one entry for '$archiveName'."
}
[void]($checksumLines[0] -match '^(?<hash>[0-9a-fA-F]{64}) ')
$expectedHash = $Matches.hash.ToLowerInvariant()
$actualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $archivePath).
    Hash.ToLowerInvariant()
if ($actualHash -cne $expectedHash) {
    throw "SHA-256 mismatch for '$archiveName': $actualHash != $expectedHash."
}
Write-Host "  [ OK ] final ZIP matches CHECKSUMS.sha256"

$identity = '^https://github.com/' + [regex]::Escape($Repository) +
    '/.github/workflows/release.yml@refs/tags/v[0-9].*$'
cosign verify-blob `
    --bundle $bundlePath `
    --certificate-oidc-issuer 'https://token.actions.githubusercontent.com' `
    --certificate-identity-regexp $identity `
    $archivePath | Out-Host
if ($LASTEXITCODE -ne 0) {
    throw "Sigstore verification failed for '$archiveName'."
}
Write-Host '  [ OK ] keyless Sigstore identity verified'

gh attestation verify $archivePath `
    --repo $Repository `
    --signer-workflow "$Repository/.github/workflows/release.yml" `
    --source-ref "refs/tags/$Tag" `
    --predicate-type 'https://slsa.dev/provenance/v1' `
    --deny-self-hosted-runners | Out-Host
if ($LASTEXITCODE -ne 0) {
    throw "SLSA provenance verification failed for '$archiveName'."
}
Write-Host '  [ OK ] GitHub SLSA provenance verified'

$destination = [System.IO.Path]::GetFullPath($DestinationDirectory)
if (Test-Path -LiteralPath $destination) {
    throw "Refusing to extract over existing path '$destination'."
}
New-Item -ItemType Directory -Path $destination | Out-Null
Expand-Archive -LiteralPath $archivePath -DestinationPath $destination
$prefix = Join-Path $destination $name
if (-not (Test-Path -LiteralPath $prefix -PathType Container)) {
    throw "The archive did not contain its canonical root '$name'."
}

$kind = $Product -replace '^ayther-', ''
$payloadArguments = @{ Directory = $prefix; Kind = $kind }
if ($kind -eq 'engine-vpx' -and $Platform -eq 'windows-x86_64') {
    $payloadArguments.RequireBundledVpx = $true
}
& $payloadCheckerPath @payloadArguments

# Keep stdout unambiguous for callers that capture the resolved prefix.
Write-Output (Resolve-Path -LiteralPath $prefix).Path

