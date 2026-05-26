# deploy-sd-plugin.ps1 - Post-build: patch ISS, stop Stream Deck, deploy plugin, restart
# Called by StreamDeckPlugin post-build event with:
#   deploy-sd-plugin.ps1 -ProjectDir <dir> -OutDir <dir>

param(
    [Parameter(Mandatory)][string]$ProjectDir,
    [Parameter(Mandatory)][string]$OutDir
)

$ErrorActionPreference = 'Stop'

function Get-PluginVersion {
    param([Parameter(Mandatory)][string]$VersionFile)

    $match = Select-String -Path $VersionFile -Pattern '#define PLUGIN_VERSION "(.+?)"'
    if (-not $match.Matches.Count) {
        throw "Could not read plugin version from $VersionFile"
    }

    return $match.Matches[0].Groups[1].Value
}

function Get-StreamDeckExecutablePath {
    $sdProc = Get-Process StreamDeck -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($sdProc -and $sdProc.Path -and (Test-Path $sdProc.Path)) {
        return $sdProc.Path
    }

    foreach ($candidate in @(
        "$env:ProgramFiles\Elgato\StreamDeck\StreamDeck.exe",
        "${env:ProgramFiles(x86)}\Elgato\StreamDeck\StreamDeck.exe",
        "$env:LOCALAPPDATA\Elgato\StreamDeck\StreamDeck.exe"
    )) {
        if ($candidate -and (Test-Path $candidate)) {
            return $candidate
        }
    }

    return ""
}

function Get-InnoSetupCompilerPath {
    foreach ($candidate in @(
        "$env:ProgramFiles(x86)\Inno Setup 6\ISCC.exe",
        "$env:ProgramFiles\Inno Setup 6\ISCC.exe"
    )) {
        if ($candidate -and (Test-Path $candidate)) {
            return $candidate
        }
    }

    return ""
}

function Copy-PluginPackage {
    param(
        [Parameter(Mandatory)][string]$SourceBase,
        [Parameter(Mandatory)][string]$BuiltExe,
        [Parameter(Mandatory)][string]$DestinationRoot
    )

    if (Test-Path $DestinationRoot) {
        Remove-Item $DestinationRoot -Recurse -Force
    }

    New-Item -Path $DestinationRoot -ItemType Directory -Force | Out-Null
    Copy-Item $BuiltExe $DestinationRoot -Force
    Copy-Item (Join-Path $SourceBase 'manifest.json') $DestinationRoot -Force
    Copy-Item (Join-Path $SourceBase 'property_inspector.html') $DestinationRoot -Force
    Copy-Item (Join-Path $SourceBase 'images') (Join-Path $DestinationRoot 'images') -Recurse -Force
}

$versionFile = Join-Path $ProjectDir 'PluginVersion.h'
$srcBase = Join-Path $ProjectDir 'com.ahlbornbridge.organ.sdPlugin'
$builtExe = Join-Path $OutDir 'AhlbornBridgeSD.exe'
$packagedPluginDir = Join-Path $OutDir 'com.ahlbornbridge.organ.sdPlugin'
$zipPath = Join-Path $OutDir 'AhlbornBridgeSD2.sdPlugin.zip'
$issSource = Join-Path $ProjectDir 'AhlbornBridgeSD.iss'
$issPatched = Join-Path $OutDir 'AhlbornBridgeSD.iss'

if (-not (Test-Path $versionFile)) { throw "Missing $versionFile" }
if (-not (Test-Path $srcBase)) { throw "Missing plugin package source folder $srcBase" }
if (-not (Test-Path $builtExe)) { throw "Missing built plugin executable $builtExe" }

$v = Get-PluginVersion -VersionFile $versionFile

# --- 1. Patch ISS installer script ---
$iss = Get-Content $issSource -Raw
$iss = $iss -replace '@PLUGIN_VERSION@', $v
$iss = $iss.Replace('{#SourceBase}', $srcBase)
Set-Content $issPatched $iss -NoNewline
Write-Host "Patched ISS with plugin version $v"

# --- 2. Create deployable plugin package in the build output ---
Copy-PluginPackage -SourceBase $srcBase -BuiltExe $builtExe -DestinationRoot $packagedPluginDir
Write-Host "Created deployable package folder $packagedPluginDir"

if (Test-Path $zipPath) {
    Remove-Item $zipPath -Force
}
Compress-Archive -Path $packagedPluginDir -DestinationPath $zipPath -CompressionLevel Optimal
Write-Host "Created plugin archive $zipPath"

# --- 3. Compile installer if Inno Setup is available ---
$isccPath = Get-InnoSetupCompilerPath
if ($isccPath) {
    Write-Host "Building Inno Setup package with $isccPath"
    & $isccPath $issPatched | Out-Host
} else {
    Write-Host 'Inno Setup compiler not found - skipped installer build'
}

# --- 4. Deploy to the local Stream Deck plugin folder if available ---
$pluginsRoot = Join-Path ([Environment]::GetFolderPath('ApplicationData')) 'Elgato\StreamDeck\Plugins'
$dest = Join-Path $pluginsRoot 'com.ahlbornbridge.organ.sdPlugin'
if (-not (Test-Path $pluginsRoot)) {
    Write-Host "Stream Deck plugins folder not found at $pluginsRoot - skipped local deployment"
    exit 0
}

$sdPath = Get-StreamDeckExecutablePath
$wasRunning = $false
if (Get-Process StreamDeck -ErrorAction SilentlyContinue) {
    Write-Host 'Stopping Stream Deck...'
    Stop-Process -Name StreamDeck -Force -ErrorAction SilentlyContinue
    $wasRunning = $true
    Start-Sleep -Seconds 2
}
if (Get-Process AhlbornBridgeSD -ErrorAction SilentlyContinue) {
    Stop-Process -Name AhlbornBridgeSD -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500
}

Copy-PluginPackage -SourceBase $srcBase -BuiltExe $builtExe -DestinationRoot $dest
Write-Host "Deployed plugin v$v to $dest"

if ($wasRunning -and $sdPath -and (Test-Path $sdPath)) {
    Write-Host "Restarting Stream Deck from $sdPath"
    Start-Process $sdPath
}

exit 0
