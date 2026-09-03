<#
.SYNOPSIS
    Verifies that an installed or unpacked AYTHER release tree holds exactly the
    payload its name promises.

.DESCRIPTION
    This is the gate behind the release-scope decision: a core-only package must
    not be publishable as a complete engine. So the check runs in both
    directions -- every expected file must be present, and for the core kind the
    engine surface must be demonstrably ABSENT. A missing shader or a stray
    engine archive both fail.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Directory,

    [Parameter(Mandatory = $true)]
    [ValidateSet('core', 'engine', 'engine-vpx')]
    [string]$Kind,

    # Windows builds and bundles libvpx via tools/build_libvpx.ps1, so the
    # archive, headers, and notices ship inside the package. Linux links the
    # system libvpx through pkg-config and ships none of it.
    [switch]$RequireBundledVpx
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = (Resolve-Path -LiteralPath $Directory).Path
$failures = [System.Collections.Generic.List[string]]::new()
$checked = 0

function Test-Payload {
    param([string]$Pattern, [string]$What)

    $script:checked++
    $matched = @(Get-ChildItem -Path (Join-Path $root $Pattern) -ErrorAction SilentlyContinue)
    if ($matched.Count -eq 0) {
        $script:failures.Add("MISSING  $What  ($Pattern)")
        return
    }
    Write-Host ("  [ OK ] {0}" -f $What)
}

function Test-Absent {
    param([string]$Pattern, [string]$What)

    $script:checked++
    $matched = @(Get-ChildItem -Path (Join-Path $root $Pattern) -ErrorAction SilentlyContinue)
    if ($matched.Count -ne 0) {
        $names = ($matched | ForEach-Object Name) -join ', '
        $script:failures.Add("PRESENT  $What must NOT ship in a core-only package: $names")
        return
    }
    Write-Host ("  [ OK ] absent, as a core-only package requires: {0}" -f $What)
}

Write-Host "=== release payload: $Kind === $root"

# --- The surface every package shares -------------------------------------
Test-Payload 'include/ayther/ayther_core_ffi.h' 'core C ABI header'
Test-Payload 'include/ayther/ayther_version.h'  'version contract header'

# The archive prefix/suffix is toolchain-specific (ayther_core.lib vs
# libayther_core.a), so match on the stem instead of guessing the decoration.
function Test-Archive {
    param([string]$Stem, [string]$What)

    $script:checked++
    $found = @(Get-ChildItem -Path (Join-Path $root 'lib') -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match "^(lib)?$Stem\.(lib|a)$" })
    if ($found.Count -eq 0) {
        $script:failures.Add("MISSING  $What (lib/[lib]$Stem.{lib,a})")
        return
    }
    Write-Host ("  [ OK ] {0}: {1}" -f $What, $found[0].Name)
}

Test-Archive 'ayther_core' 'core static archive'

Test-Payload 'lib/cmake/Ayther/AytherConfig.cmake'        'package config'
Test-Payload 'lib/cmake/Ayther/AytherConfigVersion.cmake' 'package version file'
Test-Payload 'share/licenses/Ayther/LICENSE'              'project licence'
Test-Payload 'share/licenses/Ayther/NOTICE.md'            'dependency notice'

$engineHeaders = @(
    'audio_asset_level.h', 'audio_match_rule.h', 'ayther_animation.h',
    'ayther_audio_events.h', 'ayther_layers.h', 'ayther_mode3.h',
    'ayther_renderer.h', 'ayther_result.h', 'ayther_sdk.h',
    'ayther_sdk_version.h', 'ayther_session.h', 'log.h',
    'engine/capabilities.hpp', 'engine/core_probe.hpp', 'engine/engine.hpp',
    'engine/input.hpp', 'engine/pack.hpp', 'engine/vulkan_interop.hpp')

$engineShaders = @(
    'indexed_plane.frag.spv', 'indexed_plane.vert.spv',
    'sprite.frag.spv', 'sprite.vert.spv', 'sprite_mask.frag.spv',
    'sprite_mult.frag.spv', 'sprite_screen.frag.spv', 'video.frag.spv')

if ($Kind -eq 'core') {
    # --- Proving the negative: this is NOT an engine package --------------
    Test-Absent 'lib/*ayther_engine.*'                  'the engine archive'
    Test-Absent 'lib/*ayther_ymfm.*'                    'the ymfm archive'
    Test-Absent 'lib/cmake/Ayther/AytherEngineTargets.cmake' 'the engine target export'
    Test-Absent 'share/Ayther/shaders'                  'compiled shaders'
    Test-Absent 'include/ayther/third_party'            'vendored engine headers'
    foreach ($header in $engineHeaders) {
        Test-Absent "include/ayther/$header" "engine header $header"
    }
} else {
    # --- Targets ----------------------------------------------------------
    Test-Archive 'ayther_engine' 'engine static archive'
    Test-Archive 'ayther_ymfm'   'ymfm static archive'

    Test-Payload 'lib/cmake/Ayther/AytherEngineTargets.cmake' 'engine target export'

    # --- Headers ----------------------------------------------------------
    foreach ($header in $engineHeaders) {
        Test-Payload "include/ayther/$header" "engine header $header"
    }
    Test-Payload 'include/ayther/third_party/ymfm/*.h' 'vendored ymfm headers'

    # --- Shaders ----------------------------------------------------------
    foreach ($shader in $engineShaders) {
        Test-Payload "share/Ayther/shaders/$shader" "compiled shader $shader"
    }

    # --- Dependency notices ----------------------------------------------
    Test-Payload 'share/licenses/Ayther/ymfm/LICENSE' 'ymfm licence'
    Test-Payload 'share/licenses/Ayther/vcpkg/*.txt'  'vcpkg dependency licences'

    if ($Kind -eq 'engine-vpx' -and $RequireBundledVpx) {
        $checked++
        $vpxArchive = @(Get-ChildItem -Path (Join-Path $root 'lib') -File -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match 'vpx.*\.(lib|a)$' })
        if ($vpxArchive.Count -eq 0) {
            $failures.Add('MISSING  bundled libvpx archive (lib/*vpx*.{lib,a})')
        } else {
            Write-Host ("  [ OK ] bundled libvpx archive: {0}" -f $vpxArchive[0].Name)
        }
        Test-Payload 'include/vpx/*.h'                          'libvpx headers'
        Test-Payload 'share/licenses/Ayther/libvpx/LICENSE'     'libvpx licence'
        Test-Payload 'share/licenses/Ayther/libvpx/PATENTS'     'libvpx patent grant'
    }
}

Write-Host ''
if ($failures.Count -gt 0) {
    foreach ($failure in $failures) { Write-Host "  [FAIL] $failure" }
    throw "Release payload '$Kind' failed $($failures.Count) of $checked checks."
}
Write-Host "Release payload '$Kind' verified: $checked checks passed."
