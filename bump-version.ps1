# bump-version.ps1 - Auto-increment version in Version.h
# Usage:
#   .\bump-version.ps1          -> increments patch  (0.2.3 -> 0.2.4)
#   .\bump-version.ps1 minor    -> increments minor  (0.2.3 -> 0.3.0)
#   .\bump-version.ps1 major    -> increments major  (0.2.3 -> 1.0.0)

param([string]$Part = "patch")

$file = Join-Path $PSScriptRoot "Test1\Version.h"

if (-not (Test-Path $file)) {
    Write-Warning "Version.h not found at $file - skipping bump."
    exit 0
}

$content = [System.IO.File]::ReadAllText($file, [System.Text.Encoding]::UTF8)

if (-not $content -or $content.Trim().Length -eq 0) {
    Write-Warning "Version.h is empty - skipping bump to avoid corruption."
    exit 0
}

if ($content -match '(?m)^(<<<<<<<|=======|>>>>>>>)') {
    Write-Warning "Version.h contains merge conflict markers - skipping bump."
    exit 0
}

# Read version from APP_VERSION string (the single source of truth).
$verMatch = [regex]::Match($content, '#define APP_VERSION "(\d+)\.(\d+)\.(\d+)"')
if (-not $verMatch.Success) {
    Write-Warning "Could not parse APP_VERSION from Version.h - skipping bump."
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

$content = $content -replace '#define APP_VERSION_MAJOR \d+', "#define APP_VERSION_MAJOR $major"
$content = $content -replace '#define APP_VERSION_MINOR \d+', "#define APP_VERSION_MINOR $minor"
$content = $content -replace '#define APP_VERSION_PATCH \d+', "#define APP_VERSION_PATCH $patch"
$content = $content -replace '#define APP_VERSION ".*?"', "#define APP_VERSION ""$new"""

# Atomic write: write to temp file first, then replace the original.
# This prevents Version.h from being left empty if the process is interrupted.
$tmpFile = "$file.tmp"
[System.IO.File]::WriteAllText($tmpFile, $content, [System.Text.UTF8Encoding]::new($false))
[System.IO.File]::Delete($file)
[System.IO.File]::Move($tmpFile, $file)

Write-Host "Version bumped: $old -> $new"
