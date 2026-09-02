<#
.SYNOPSIS
    Writes the release body for a tagged AYTHER pre-release.

.DESCRIPTION
    v0.1.x publishes two Engine variants on Windows and Linux. The notes are
    generated rather than hand-written so the scope statement cannot drift away
    from the four archives the workflow actually built.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^v\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$')]
    [string]$Tag,

    [Parameter(Mandatory = $true)]
    [string]$OutputFile,

    [string]$Repository = 'Ayther-Dev/AYTHER-Engine'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$lines = @(
    "# AYTHER $Tag",
    '',
    '> [!WARNING]',
    '> This is a **pre-release**. No stable or supported artifact exists yet.',
    '> The physical GPU/driver matrix, real-emulator fixtures, and the remaining',
    '> security review are still open. Treat every archive here as a candidate to',
    '> evaluate, not a supported dependency.',
    '',
    '## Which archive do I want?',
    '',
    'One tag, two Engine variants, on Windows and Linux:',
    '',
    '| Archive | Contains | Does **not** contain |',
    '| --- | --- | --- |',
    ("| ``ayther-engine-$Tag-<platform>.zip`` | ``Ayther::core``, " +
     '`Ayther::engine`, `Ayther::ymfm`, the public engine headers, and the ' +
     'compiled SPIR-V shaders | VP9 video decoding |'),
    ("| ``ayther-engine-vpx-$Tag-<platform>.zip`` | Everything in engine, **plus** the " +
     'VP9 decoder | -- |'),
    '',
    'Every archive contains the complete Engine package, including its Core',
    'archive. Every archive in this release was built, tested, unpacked,',
    'payload-checked, and consumed by an out-of-tree',
    'CMake project before publication; an archive whose payload did not match its',
    'name would have failed the release.',
    '',
    '## Verifying an archive',
    '',
    'Three independent records cover every archive. Check all of them:',
    '',
    '```text',
    'sha256sum --check CHECKSUMS.sha256',
    '',
    "gh attestation verify ayther-engine-$Tag-linux-x86_64.zip \",
    "  --repo $Repository",
    '',
    'cosign verify-blob \',
    "  --bundle ayther-engine-$Tag-linux-x86_64.zip.sigstore.json \",
    '  --certificate-oidc-issuer https://token.actions.githubusercontent.com \',
    ("  --certificate-identity-regexp '^https://github.com/$Repository" +
     "/.github/workflows/release.yml@refs/tags/v[0-9].*`$' \"),
    "  ayther-engine-$Tag-linux-x86_64.zip",
    '```',
    '',
    'Each archive ships an SPDX 2.3 SBOM generated from the locked Cargo graph and',
    'the exact installed files, signed and attested alongside it.',
    '',
    '## Consuming an archive',
    '',
    'Unpack it and point `CMAKE_PREFIX_PATH` at the unpacked directory.',
    '',
    '```cmake',
    'find_package(Ayther 0.1.0 CONFIG REQUIRED COMPONENTS engine)',
    'target_link_libraries(my_app PRIVATE Ayther::engine)',
    '```',
    '',
    'An engine consumer resolves SDL3, Vulkan, VulkanMemoryAllocator,',
    'toml++, and zstd from its own package-manager environment.',
    '',
    '## Changelog',
    '',
    "See [CHANGELOG.md](https://github.com/$Repository/blob/$Tag/CHANGELOG.md) and",
    "[docs/BUILD_TEST_RELEASE.md](https://github.com/$Repository/blob/$Tag/docs/BUILD_TEST_RELEASE.md)."
)

$outputDirectory = Split-Path -Parent ([System.IO.Path]::GetFullPath($OutputFile))
if ($outputDirectory -and -not (Test-Path -LiteralPath $outputDirectory)) {
    New-Item -ItemType Directory -Path $outputDirectory | Out-Null
}

[System.IO.File]::WriteAllLines(
    [System.IO.Path]::GetFullPath($OutputFile),
    $lines,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Host "Wrote release notes for $Tag to '$OutputFile'."
