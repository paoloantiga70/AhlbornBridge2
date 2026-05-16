# deploy-sd-plugin.ps1 - Post-build: patch ISS, stop Stream Deck, deploy plugin, restart
# Called by StreamDeckPlugin post-build event with:
#   deploy-sd-plugin.ps1 -ProjectDir <dir> -OutDir <dir>

param(
    [Parameter(Mandatory)][string]$ProjectDir,
    [Parameter(Mandatory)][string]$OutDir
)

# --- 1. Patch ISS installer script ---
$versionFile = Join-Path $ProjectDir "PluginVersion.h"
$v = (Select-String -Path $versionFile -Pattern '#define PLUGIN_VERSION "(.+?)"').Matches[0].Groups[1].Value
$iss = Get-Content (Join-Path $ProjectDir "AhlbornBridgeSD.iss") -Raw
$iss = $iss -replace '@PLUGIN_VERSION@', $v
$srcBase = Join-Path $ProjectDir "com.ahlbornbridge.organ.sdPlugin"
$iss = $iss -replace '\{#SourceBase\}', $srcBase
Set-Content (Join-Path $OutDir "AhlbornBridgeSD.iss") $iss -NoNewline
Write-Host "Patched ISS with plugin version $v"

# --- 2. Deploy to Stream Deck plugin folder ---
$dest = [Environment]::GetFolderPath('ApplicationData') + '\Elgato\StreamDeck\Plugins\com.ahlbornbridge.organ.sdPlugin'
if (-not (Test-Path $dest)) {
    Write-Host "Plugin folder not found at $dest - skipping deploy"
    exit 0
}

# Remember StreamDeck.exe path before killing it
$sdPath = ""
$sdProc = Get-Process StreamDeck -ErrorAction SilentlyContinue | Select-Object -First 1
if ($sdProc) {
    $sdPath = $sdProc.Path
}
if (-not $sdPath -or -not (Test-Path $sdPath)) {
    # Search common install locations
    @(
        "$env:ProgramFiles\Elgato\StreamDeck\StreamDeck.exe",
        "${env:ProgramFiles(x86)}\Elgato\StreamDeck\StreamDeck.exe",
        "$env:LOCALAPPDATA\Elgato\StreamDeck\StreamDeck.exe"
    ) | ForEach-Object { if (Test-Path $_) { $sdPath = $_; return } }
}

# Stop Stream Deck + plugin process so files aren't locked
$wasRunning = $false
if (Get-Process StreamDeck -ErrorAction SilentlyContinue) {
    Write-Host "Stopping Stream Deck..."
    Stop-Process -Name StreamDeck -Force -ErrorAction SilentlyContinue
    $wasRunning = $true
    Start-Sleep -Seconds 2
}
if (Get-Process AhlbornBridgeSD -ErrorAction SilentlyContinue) {
    Stop-Process -Name AhlbornBridgeSD -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500
}

# Copy files
Copy-Item (Join-Path $OutDir "AhlbornBridgeSD.exe") $dest -Force
Copy-Item (Join-Path $srcBase "manifest.json") $dest -Force
Copy-Item (Join-Path $srcBase "property_inspector.html") $dest -Force
$imagesDir = Join-Path $dest "images"
if (-not (Test-Path $imagesDir)) { New-Item $imagesDir -ItemType Directory -Force | Out-Null }
Copy-Item (Join-Path $srcBase "images\*") $imagesDir -Force -Recurse
Write-Host "Deployed plugin v$v to $dest"

# Restart Stream Deck if it was running
if ($wasRunning -and $sdPath -and (Test-Path $sdPath)) {
    Write-Host "Restarting Stream Deck from $sdPath"
    Start-Process $sdPath
}

exit 0
