<#
.SYNOPSIS
    Writes the release body for a tagged AYTHER pre-release.

.DESCRIPTION
    v0.1.x publishes three artifact families from one tag. The notes are
    generated rather than hand-written so the scope statement cannot drift away
    from what the workflow actually built: a reader must be able to tell, from
    the release page alone, that ayther-core is NOT the complete engine.
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
    'One tag, three artifact families, per platform:',
    '',
    '| Archive | Contains | Does **not** contain |',
    '| --- | --- | --- |',
    ("| ``ayther-core-$Tag-<platform>.zip`` | ``Ayther::core`` static archive, the C ABI " +
     'headers, and the CMake package | The engine, the renderer, shaders, or any ' +
     'SDL3/Vulkan dependency |'),
    ("| ``ayther-engine-$Tag-<platform>.zip`` | Everything in core, **plus** " +
     '`Ayther::engine`, `Ayther::ymfm`, the public engine headers, and the ' +
     'compiled SPIR-V shaders | VP9 video decoding |'),
    ("| ``ayther-engine-vpx-$Tag-<platform>.zip`` | Everything in engine, **plus** the " +
     'VP9 decoder | -- |'),
    '',
    '**The `ayther-core` archive is not the AYTHER Engine.** It is the core SDK on',
    'its own: it links without SDL3 or Vulkan and renders nothing. If you want the',
    'engine, take an `ayther-engine` archive. Every archive in this release was',
    'built, tested, unpacked, payload-checked, and consumed by an out-of-tree',
    'CMake project before publication; an archive whose payload did not match its',
    'name would have failed the release.',
    '',
    '## Verifying an archive',
    '',
    'Three independent records cover every asset. Check all of them:',
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
    '# ayther-core',
    'find_package(Ayther 0.1 CONFIG REQUIRED)',
    'target_link_libraries(my_app PRIVATE Ayther::core)',
    '',
    '# ayther-engine / ayther-engine-vpx',
    'find_package(Ayther 0.1 CONFIG REQUIRED COMPONENTS engine)',
    'target_link_libraries(my_app PRIVATE Ayther::engine)',
    '```',
    '',
    'An engine consumer resolves SDL3, Vulkan, VulkanMemoryAllocator,',
    'vk-bootstrap, toml++, and zstd from its own package-manager environment.',
    'A core consumer needs none of them.',
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
