[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$WorkDirectory,

    [Parameter(Mandatory = $true)]
    [string]$Scanner,

    [Parameter(Mandatory = $true)]
    [string]$Workflow,

    [Parameter(Mandatory = $true)]
    [string]$RuntimeWorkflow,

    [Parameter(Mandatory = $true)]
    [string]$Verifier,

    [Parameter(Mandatory = $true)]
    [string]$Packager
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = [System.IO.Path]::GetFullPath($WorkDirectory)
$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$repositoryPrefix = $repositoryRoot.TrimEnd(
    [System.IO.Path]::DirectorySeparatorChar,
    [System.IO.Path]::AltDirectorySeparatorChar
) + [System.IO.Path]::DirectorySeparatorChar
if (-not $root.StartsWith(
        $repositoryPrefix,
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "External-consumer test workspace must stay inside '$repositoryRoot'."
}
if (Test-Path -LiteralPath $root) {
    Remove-Item -LiteralPath $root -Recurse -Force
}
New-Item -ItemType Directory -Path $root | Out-Null

$producerRoot = Join-Path $root 'producer-checkout'
$cacheA = Join-Path $root 'minimal/CMakeCache.txt'
$cacheB = Join-Path $root 'runtime/CMakeCache.txt'
$report = Join-Path $root 'minimal/imported-targets.txt'
foreach ($directory in (Split-Path -Parent $cacheA), (Split-Path -Parent $cacheB)) {
    New-Item -ItemType Directory -Path $directory | Out-Null
}

function Write-Utf8File {
    param([string]$Path, [string]$Contents)
    [System.IO.File]::WriteAllText(
        $Path, $Contents, [System.Text.UTF8Encoding]::new($false))
}

function Assert-ScannerRejects {
    param([string]$ExpectedMessage)

    $accepted = $false
    try {
        & $Scanner `
            -CacheFiles $cacheA, $cacheB `
            -ImportedTargetsReport $report `
            -ForbiddenRoots $producerRoot
        $accepted = $true
    }
    catch {
        if ($_.Exception.Message -cne $ExpectedMessage) {
            throw
        }
    }
    if ($accepted) {
        throw 'The external-consumer path scanner accepted a producer path.'
    }
}

Write-Utf8File $cacheA 'CMAKE_HOME_DIRECTORY:INTERNAL=C:/temp/minimal-source'
Write-Utf8File $cacheB 'CMAKE_HOME_DIRECTORY:INTERNAL=C:/temp/runtime-source'
Write-Utf8File $report 'Ayther::engine|IMPORTED_LOCATION|C:/temp/prefix/lib/engine.lib'
& $Scanner `
    -CacheFiles $cacheA, $cacheB `
    -ImportedTargetsReport $report `
    -ForbiddenRoots $producerRoot

$forwardProducerRoot = $producerRoot.Replace('\', '/')
Write-Utf8File $cacheB "LEAK:FILEPATH=$forwardProducerRoot/build/engine.lib"
Assert-ScannerRejects `
    'External consumer retained a producer checkout, build, or monorepo path.'

Write-Utf8File $cacheB 'CMAKE_HOME_DIRECTORY:INTERNAL=C:/temp/runtime-source'
$backslashProducerRoot = $producerRoot.Replace('/', '\')
Write-Utf8File $report (
    "Ayther::engine|INTERFACE_INCLUDE_DIRECTORIES|" +
    "$backslashProducerRoot\include")
Assert-ScannerRejects `
    'External consumer retained a producer checkout, build, or monorepo path.'

Write-Host 'External-consumer path gate contract passed.'

$workflowText = [System.IO.File]::ReadAllText(
    (Resolve-Path -LiteralPath $Workflow).Path).Replace("`r`n", "`n")
$buildOffset = $workflowText.IndexOf("`n  build:`n", [StringComparison]::Ordinal)
$attestOffset = $workflowText.IndexOf("`n  attest:`n", [StringComparison]::Ordinal)
$consumerOffset = $workflowText.IndexOf(
    "`n  package-consumer:`n", [StringComparison]::Ordinal)
$publishOffset = $workflowText.IndexOf("`n  publish:`n", [StringComparison]::Ordinal)
if ($buildOffset -lt 0 -or $attestOffset -le $buildOffset -or
    $consumerOffset -le $attestOffset -or
    $publishOffset -le $consumerOffset) {
    throw 'Release jobs must be ordered build -> attest -> package-consumer -> publish.'
}

$buildJob = $workflowText.Substring($buildOffset, $attestOffset - $buildOffset)
foreach ($forbiddenBuildStep in @(
        'external_package_consumer',
        'Configure the minimal consumer',
        'Configure Runtime')) {
    if ($buildJob.Contains($forbiddenBuildStep, [StringComparison]::Ordinal)) {
        throw "The producer build job must not consume its own artifact ('$forbiddenBuildStep')."
    }
}

$attestJob = $workflowText.Substring(
    $attestOffset, $consumerOffset - $attestOffset)
foreach ($requiredAttestStep in @(
        'needs: build',
        'Download immutable build outputs',
        'Upload the immutable attested candidate')) {
    if (-not $attestJob.Contains($requiredAttestStep, [StringComparison]::Ordinal)) {
        throw "Attestation stage is missing '$requiredAttestStep'."
    }
}

$externalJob = $workflowText.Substring(
    $consumerOffset, $publishOffset - $consumerOffset)
$requiredExternalSteps = @(
    'needs: attest',
    'Download the attested packaged artifacts',
    'Verify checksum, signature, provenance, and payload',
    'Configure the minimal consumer from the package prefix',
    'Build and run the minimal consumer',
    'Reject producer paths',
    '-DVCPKG_MANIFEST_DIR=$env:MINIMAL_SOURCE',
    "-ForbiddenRoots '`${{ github.workspace }}'")
foreach ($requiredStep in $requiredExternalSteps) {
    if (-not $externalJob.Contains($requiredStep, [StringComparison]::Ordinal)) {
        throw "External-consumer workflow is missing '$requiredStep'."
    }
}

foreach ($forbiddenRuntimeReference in @(
        'AYTHER_RUNTIME_REPOSITORY',
        'AYTHER_RUNTIME_REF',
        'Check out the pinned independent Runtime',
        'Configure Runtime',
        'Build Runtime',
        'Run Runtime CTest')) {
    if ($workflowText.Contains(
            $forbiddenRuntimeReference, [StringComparison]::Ordinal)) {
        throw "Release workflow must not depend on Runtime ('$forbiddenRuntimeReference')."
    }
}

$matrixExpectations = @{
    '(?m)^          - product: ayther-engine$' = 2
    '(?m)^          - product: ayther-engine-vpx$' = 2
    '(?m)^            platform: linux-x86_64$' = 2
    '(?m)^            platform: windows-x86_64$' = 2
}
foreach ($expectation in $matrixExpectations.GetEnumerator()) {
    $actual = [regex]::Matches($externalJob, $expectation.Key).Count
    if ($actual -ne $expectation.Value) {
        throw "External-consumer matrix '$($expectation.Key)' has $actual entries; expected $($expectation.Value)."
    }
}

$publishJob = $workflowText.Substring($publishOffset)
if (-not $publishJob.Contains('needs: package-consumer', [StringComparison]::Ordinal)) {
    throw 'Publication must depend on the complete package-consumer matrix.'
}

Write-Host 'Package-consumer release topology contract passed.'

$runtimeWorkflowText = [System.IO.File]::ReadAllText(
    (Resolve-Path -LiteralPath $RuntimeWorkflow).Path).Replace("`r`n", "`n")
foreach ($requiredRuntimeStep in @(
        'workflow_run:',
        '- Release',
        '- completed',
        'workflow_dispatch:',
        'AYTHER_RUNTIME_REPOSITORY: Ayther-Dev/AYTHER-Runtime',
        'GH_TOKEN: ${{ github.token }}',
        'Check out the exact published Engine tag',
        'Check out the pinned independent Runtime',
        'Verify and unpack the published Engine package',
        'Configure Runtime against the published Engine package',
        'Build Runtime',
        'Run Runtime CTest')) {
    if (-not $runtimeWorkflowText.Contains(
            $requiredRuntimeStep, [StringComparison]::Ordinal)) {
        throw "Runtime integration workflow is missing '$requiredRuntimeStep'."
    }
}
if (-not $runtimeWorkflowText.Contains(
        "github.event.workflow_run.conclusion == 'success'",
        [StringComparison]::Ordinal)) {
    throw 'Automatic Runtime integration must require a successful Release workflow.'
}
foreach ($forbiddenRuntimePermission in @('environment: release', 'contents: write')) {
    if ($runtimeWorkflowText.Contains(
            $forbiddenRuntimePermission, [StringComparison]::Ordinal)) {
        throw "Runtime integration must not publish releases ('$forbiddenRuntimePermission')."
    }
}

Write-Host 'Independent Runtime integration topology contract passed.'

$verifierText = [System.IO.File]::ReadAllText(
    (Resolve-Path -LiteralPath $Verifier).Path).Replace("`r`n", "`n")
$verificationOffsets = @(
    $verifierText.IndexOf('$actualHash =', [StringComparison]::Ordinal),
    $verifierText.IndexOf('cosign verify-blob', [StringComparison]::Ordinal),
    $verifierText.IndexOf('gh attestation verify', [StringComparison]::Ordinal),
    $verifierText.IndexOf('Expand-Archive', [StringComparison]::Ordinal))
for ($index = 0; $index -lt $verificationOffsets.Count; ++$index) {
    if ($verificationOffsets[$index] -lt 0 -or
        ($index -gt 0 -and
         $verificationOffsets[$index] -le $verificationOffsets[$index - 1])) {
        throw 'Candidate verification must run checksum -> Sigstore -> provenance -> extraction.'
    }
}
Write-Host 'Staged-candidate verification order contract passed.'

# Exercise the staged-candidate verifier without network access. The command
# identities are mocked, but the checksum, canonical archive root, extraction,
# payload-checker handoff, and single-prefix output are real.
$candidateDirectory = Join-Path $root 'candidate'
$packageInput = Join-Path $root 'candidate-input'
$tag = 'v0.1.0-rc.5'
$product = 'ayther-engine'
$platform = 'windows-x86_64'
$archiveRoot = "$product-$tag-$platform"
$archive = Join-Path $candidateDirectory "$archiveRoot.zip"
$payloadConfig = Join-Path $packageInput 'lib/cmake/Ayther/AytherConfig.cmake'
New-Item -ItemType Directory -Path (Split-Path -Parent $payloadConfig) -Force |
    Out-Null
Write-Utf8File $payloadConfig '# mock installed package'
& $Packager `
    -InputDirectory $packageInput `
    -OutputFile $archive `
    -ArchiveRoot $archiveRoot `
    -SourceDateEpoch 1788378381
$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash.
    ToLowerInvariant()
Write-Utf8File (Join-Path $candidateDirectory 'CHECKSUMS.sha256') `
    "$hash *$archiveRoot.zip`n"
Write-Utf8File "$archive.sigstore.json" '{}'

$mockPayloadChecker = Join-Path $root 'mock_payload_checker.ps1'
Write-Utf8File $mockPayloadChecker @'
param([string]$Directory, [string]$Kind, [switch]$RequireBundledVpx)
if ($Kind -cne 'engine') { throw "Unexpected payload kind '$Kind'." }
if (-not (Test-Path -LiteralPath (
        Join-Path $Directory 'lib/cmake/Ayther/AytherConfig.cmake'))) {
    throw 'Extracted mock package is incomplete.'
}
'@

function cosign {
    $global:LASTEXITCODE = 0
}
function gh {
    $global:LASTEXITCODE = 0
}

$verifiedPrefix = @(& $Verifier `
    -CandidateDirectory $candidateDirectory `
    -Tag $tag `
    -Product $product `
    -Platform $platform `
    -DestinationDirectory (Join-Path $root 'verified') `
    -PayloadChecker $mockPayloadChecker)
if ($verifiedPrefix.Count -ne 1 -or
    -not (Test-Path -LiteralPath $verifiedPrefix[0] -PathType Container)) {
    throw 'Staged-candidate verifier did not return one extracted prefix.'
}

[System.IO.File]::AppendAllText($archive, 'tampered')
$acceptedTamperedArchive = $false
try {
    & $Verifier `
        -CandidateDirectory $candidateDirectory `
        -Tag $tag `
        -Product $product `
        -Platform $platform `
        -DestinationDirectory (Join-Path $root 'tampered') `
        -PayloadChecker $mockPayloadChecker
    $acceptedTamperedArchive = $true
}
catch {
    if ($_.Exception.Message -notlike 'SHA-256 mismatch*') {
        throw
    }
}
if ($acceptedTamperedArchive) {
    throw 'Staged-candidate verifier accepted a ZIP after checksum tampering.'
}

Write-Host 'Staged-candidate checksum and extraction contract passed.'
