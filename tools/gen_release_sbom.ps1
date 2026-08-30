[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$InstallDirectory,

    [Parameter(Mandatory = $true)]
    [string]$OutputFile,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9._-]+$')]
    [string]$Platform,

    [Parameter(Mandatory = $true)]
    [ValidateRange(0, [long]::MaxValue)]
    [long]$SourceDateEpoch,

    [string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function ConvertTo-SpdxId([string]$Value) {
    return 'SPDXRef-' + ($Value -replace '[^A-Za-z0-9.-]', '-')
}

$root = (Resolve-Path -LiteralPath $RepositoryRoot).Path
$install = (Resolve-Path -LiteralPath $InstallDirectory).Path
$metadata = cargo metadata --manifest-path (Join-Path $root 'Cargo.toml') --locked --format-version 1 |
    ConvertFrom-Json -Depth 100
if ($LASTEXITCODE -ne 0) {
    throw 'cargo metadata failed.'
}

$workspacePackage = $metadata.packages |
    Where-Object { $_.name -eq 'ayther_core' } |
    Select-Object -First 1
if ($null -eq $workspacePackage) {
    throw 'Cargo metadata did not contain ayther_core.'
}

$packages = [System.Collections.Generic.List[object]]::new()
foreach ($package in ($metadata.packages | Sort-Object name, version)) {
    $license = if ([string]::IsNullOrWhiteSpace([string]$package.license)) {
        'NOASSERTION'
    }
    else {
        [string]$package.license
    }
    $externalRefs = @()
    if ($package.source -like 'registry+*') {
        $externalRefs = @([ordered]@{
            referenceCategory = 'PACKAGE-MANAGER'
            referenceType = 'purl'
            referenceLocator = "pkg:cargo/$($package.name)@$($package.version)"
        })
    }
    $checksums = @()
    $packageChecksum = if ($null -ne $package.PSObject.Properties['checksum']) {
        [string]$package.checksum
    }
    else {
        ''
    }
    if (-not [string]::IsNullOrWhiteSpace($packageChecksum)) {
        $checksums = @([ordered]@{
            algorithm = 'SHA256'
            checksumValue = $packageChecksum
        })
    }
    $packages.Add([ordered]@{
        SPDXID = ConvertTo-SpdxId("cargo-$($package.name)-$($package.version)")
        name = [string]$package.name
        versionInfo = [string]$package.version
        downloadLocation = if ($package.source -like 'registry+*') { 'https://crates.io/' } else { 'NOASSERTION' }
        filesAnalyzed = $false
        licenseConcluded = 'NOASSERTION'
        licenseDeclared = $license
        copyrightText = 'NOASSERTION'
        checksums = $checksums
        externalRefs = $externalRefs
    })
}

$distributionId = ConvertTo-SpdxId("ayther-engine-$($workspacePackage.version)-$Platform")
$packages.Insert(0, [ordered]@{
    SPDXID = $distributionId
    name = "AYTHER Engine $Platform distribution"
    versionInfo = [string]$workspacePackage.version
    downloadLocation = 'NOASSERTION'
    filesAnalyzed = $false
    licenseConcluded = 'MPL-2.0'
    licenseDeclared = 'MPL-2.0'
    copyrightText = 'NOASSERTION'
    checksums = @()
    externalRefs = @()
})

$files = [System.Collections.Generic.List[object]]::new()
$relationships = [System.Collections.Generic.List[object]]::new()
$fileIndex = 0
Get-ChildItem -LiteralPath $install -Recurse -File |
    Sort-Object FullName |
    ForEach-Object {
        $fileIndex += 1
        $fileId = "SPDXRef-File-$fileIndex"
        $relative = [System.IO.Path]::GetRelativePath($install, $_.FullName).Replace('\', '/')
        $files.Add([ordered]@{
            SPDXID = $fileId
            fileName = "./$relative"
            checksums = @([ordered]@{
                algorithm = 'SHA256'
                checksumValue = (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash.ToLowerInvariant()
            })
            licenseConcluded = 'NOASSERTION'
            licenseInfoInFiles = @('NOASSERTION')
            copyrightText = 'NOASSERTION'
        })
        $relationships.Add([ordered]@{
            spdxElementId = $distributionId
            relationshipType = 'CONTAINS'
            relatedSpdxElement = $fileId
        })
    }

foreach ($node in ($metadata.resolve.nodes | Sort-Object id)) {
    $fromPackage = $metadata.packages | Where-Object id -eq $node.id | Select-Object -First 1
    if ($null -eq $fromPackage) { continue }
    $fromId = ConvertTo-SpdxId("cargo-$($fromPackage.name)-$($fromPackage.version)")
    foreach ($dependencyId in ($node.dependencies | Sort-Object)) {
        $dependency = $metadata.packages | Where-Object id -eq $dependencyId | Select-Object -First 1
        if ($null -eq $dependency) { continue }
        $relationships.Add([ordered]@{
            spdxElementId = $fromId
            relationshipType = 'DEPENDS_ON'
            relatedSpdxElement = ConvertTo-SpdxId("cargo-$($dependency.name)-$($dependency.version)")
        })
    }
}
$relationships.Add([ordered]@{
    spdxElementId = $distributionId
    relationshipType = 'DEPENDS_ON'
    relatedSpdxElement = ConvertTo-SpdxId("cargo-ayther_core-$($workspacePackage.version)")
})
$relationships.Add([ordered]@{
    spdxElementId = 'SPDXRef-DOCUMENT'
    relationshipType = 'DESCRIBES'
    relatedSpdxElement = $distributionId
})

$created = [DateTimeOffset]::FromUnixTimeSeconds($SourceDateEpoch).UtcDateTime.ToString('yyyy-MM-ddTHH:mm:ssZ')
$document = [ordered]@{
    spdxVersion = 'SPDX-2.3'
    dataLicense = 'CC0-1.0'
    SPDXID = 'SPDXRef-DOCUMENT'
    name = "ayther-engine-$($workspacePackage.version)-$Platform"
    documentNamespace = "https://github.com/Ayther-Dev/AYTHER-Engine/releases/tag/v$($workspacePackage.version)/sbom/$Platform"
    creationInfo = [ordered]@{
        created = $created
        creators = @('Tool: AYTHER gen_release_sbom.ps1')
    }
    packages = $packages
    files = $files
    relationships = $relationships
}

$outputPath = [System.IO.Path]::GetFullPath($OutputFile)
$outputDirectory = Split-Path -Parent $outputPath
if (-not (Test-Path -LiteralPath $outputDirectory)) {
    New-Item -ItemType Directory -Path $outputDirectory | Out-Null
}
$json = $document | ConvertTo-Json -Depth 20
[System.IO.File]::WriteAllText($outputPath, $json + "`n", [System.Text.UTF8Encoding]::new($false))
Write-Host "Wrote SPDX 2.3 SBOM to '$outputPath'."
