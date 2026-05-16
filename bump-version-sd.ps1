# bump-version-sd.ps1 - Auto-increment version in PluginVersion.h + manifest.json
# Usage:
#   .\bump-version-sd.ps1          -> increments patch  (1.0.3 -> 1.0.4)
#   .\bump-version-sd.ps1 minor    -> increments minor  (1.0.3 -> 1.1.0)
#   .\bump-version-sd.ps1 major    -> increments major  (1.0.3 -> 2.0.0)

param([string]$Part = "patch")

$versionFile = Join-Path $PSScriptRoot "StreamDeckPlugin\PluginVersion.h"
$manifestFile = Join-Path $PSScriptRoot "StreamDeckPlugin\com.ahlbornbridge.organ.sdPlugin\manifest.json"

# --- PluginVersion.h ---
if (-not (Test-Path $versionFile)) {
    Write-Warning "PluginVersion.h not found at $versionFile - skipping bump."
    exit 0
}

$content = [System.IO.File]::ReadAllText($versionFile, [System.Text.Encoding]::UTF8)

if (-not $content -or $content.Trim().Length -eq 0) {
    Write-Warning "PluginVersion.h is empty - skipping bump to avoid corruption."
    exit 0
}

$verMatch = [regex]::Match($content, '#define PLUGIN_VERSION "(\d+)\.(\d+)\.(\d+)"')
if (-not $verMatch.Success) {
    Write-Warning "Could not parse PLUGIN_VERSION from PluginVersion.h - skipping bump."
    exit 0
}

$major = [int]$verMatch.Groups[1].Value
$minor = [int]$verMatch.Groups[2].Value
$patch = [int]$verMatch.Groups[3].Value

$old = "$major.$minor.$patch"

switch ($Part) {
    "major" { $major++; $minor = 0; $patch = 0 }
    "minor" { $minor++; $patch = 0 }
    "patch" { $patch++ }
}

$new = "$major.$minor.$patch"

$content = $content -replace '#define PLUGIN_VERSION_MAJOR \d+', "#define PLUGIN_VERSION_MAJOR $major"
$content = $content -replace '#define PLUGIN_VERSION_MINOR \d+', "#define PLUGIN_VERSION_MINOR $minor"
$content = $content -replace '#define PLUGIN_VERSION_PATCH \d+', "#define PLUGIN_VERSION_PATCH $patch"
$content = $content -replace '#define PLUGIN_VERSION ".*?"', "#define PLUGIN_VERSION ""$new"""

# Atomic write for PluginVersion.h
$tmpFile = "$versionFile.tmp"
[System.IO.File]::WriteAllText($tmpFile, $content, [System.Text.UTF8Encoding]::new($false))
[System.IO.File]::Delete($versionFile)
[System.IO.File]::Move($tmpFile, $versionFile)

# --- manifest.json ---
if (Test-Path $manifestFile) {
    $json = [System.IO.File]::ReadAllText($manifestFile, [System.Text.Encoding]::UTF8)
    $json = $json -replace '"Version":\s*".*?"', "`"Version`": `"$new`""

    $tmpManifest = "$manifestFile.tmp"
    [System.IO.File]::WriteAllText($tmpManifest, $json, [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::Delete($manifestFile)
    [System.IO.File]::Move($tmpManifest, $manifestFile)

    Write-Host "Plugin version bumped: $old -> $new (PluginVersion.h + manifest.json)"
} else {
    Write-Host "Plugin version bumped: $old -> $new (PluginVersion.h only, manifest.json not found)"
}
