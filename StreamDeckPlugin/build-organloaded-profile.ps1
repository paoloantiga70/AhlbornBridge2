# Rebuilds the OrganLoaded.streamDeckProfile package with:
#  - page 1: a "current organ" button (currentOrgan=true) + the first IR church buttons
#  - following pages: the remaining IR church buttons
# Each IR church button is a com.ahlbornbridge.ir action carrying the FRONT/SIDE/REAR
# IR_Autom OSC values and the thumbnail filename, read from churches.json.
# The package wrapper (root package.json + nested Profiles/<GUID>.sdProfile) is preserved,
# because Stream Deck only recognizes profiles exported in this exact format.

$ErrorActionPreference = 'Stop'

$PluginDir   = Join-Path $PSScriptRoot 'com.ahlbornbridge.organ.sdPlugin'
$ProfilePath = Join-Path $PluginDir   'OrganLoaded.streamDeckProfile'
$ChurchesJson = Join-Path $PSScriptRoot 'churches.json'

if (-not (Test-Path $ProfilePath)) { throw "Exported profile not found: $ProfilePath" }
if (-not (Test-Path $ChurchesJson)) { throw "churches.json not found: $ChurchesJson" }

$churches = Get-Content $ChurchesJson -Raw | ConvertFrom-Json
# Show churches in alphabetical order by name.
$churches = $churches | Sort-Object { $_.name }

# --- Device grid (Stream Deck Mobile VSD2/WiFi) --------------------------
$Cols = 5
$Rows = 3
$SlotsPerPage = $Cols * $Rows

# --- Work in a temp staging folder ---------------------------------------
$stage = Join-Path $env:TEMP ("OL_build_" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage | Out-Null
$zipCopy = Join-Path $stage 'in.zip'
Copy-Item $ProfilePath $zipCopy
$x = Join-Path $stage 'x'
Expand-Archive -Path $zipCopy -DestinationPath $x -Force

# --- Locate the nested .sdProfile ----------------------------------------
$sdProfileDir = Get-ChildItem -Path (Join-Path $x 'Profiles') -Directory -Filter '*.sdProfile' | Select-Object -First 1
if (-not $sdProfileDir) { throw 'Nested .sdProfile not found in package' }
$profileManifestPath = Join-Path $sdProfileDir.FullName 'manifest.json'
$pagesRoot = Join-Path $sdProfileDir.FullName 'Profiles'

# --- Plugin descriptor reused by every action ----------------------------
$plugin = [ordered]@{
	Name    = 'AhlbornBridge'
	UUID    = 'com.ahlbornbridge.organ'
	Version = '1.0.375'
}

function New-State {
	param([string]$titleColor = '#ffffff', [string]$alignment = 'middle', [bool]$showTitle = $true)
	[ordered]@{
		FontFamily       = ''
		FontSize         = 11
		FontStyle        = ''
		FontUnderline    = $false
		OutlineThickness = 2
		ShowTitle        = $showTitle
		TitleAlignment   = $alignment
		TitleColor       = $titleColor
	}
}

function New-CurrentOrganAction {
	[ordered]@{
		ActionID    = [guid]::NewGuid().ToString()
		LinkedTitle = $true
		Name        = 'Current Organ'
		Plugin      = $plugin
		Resources   = $null
		Settings    = [ordered]@{ currentOrgan = $true }
		State       = 0
		States      = @((New-State), (New-State))
		UUID        = 'com.ahlbornbridge.organ.load'
	}
}

function New-IrAction {
	param($church)
	[ordered]@{
		ActionID    = [guid]::NewGuid().ToString()
		LinkedTitle = $true
		Name        = $church.name
		Plugin      = $plugin
		Resources   = $null
		Settings    = [ordered]@{
			churchName = $church.name
			thumbnail  = $church.thumbnail
			oscFront   = [string]$church.front
			oscSide    = [string]$church.side
			oscRear    = [string]$church.rear
		}
		State       = 0
		States      = @((New-State '#ffffff' 'bottom' $false))
		UUID        = 'com.ahlbornbridge.ir'
	}
}

function New-FillerAction {
	# Empty IR button that paints itself black so unused slots never show a
	# ghost image from another page on the Stream Deck Mobile.
	[ordered]@{
		ActionID    = [guid]::NewGuid().ToString()
		LinkedTitle = $true
		Name        = ''
		Plugin      = $plugin
		Resources   = $null
		Settings    = [ordered]@{ churchName = ''; thumbnail = ''; oscFront = ''; oscSide = ''; oscRear = '' }
		State       = 0
		States      = @((New-State '#ffffff' 'bottom' $false))
		UUID        = 'com.ahlbornbridge.ir'
	}
}

# --- Build pages ----------------------------------------------------------
# The Stream Deck Mobile does not reliably honour Default/Current and may land
# on any page, so the Current Organ (unload) button is placed at slot 0,0 of
# EVERY page. Each page therefore holds the organ button plus up to
# (SlotsPerPage - 1) churches, padded with black fillers.
$churchesPerPage = $SlotsPerPage - 1
$allChurches = @($churches)

# Remove any pre-existing page directories so the layout is deterministic.
if (Test-Path $pagesRoot) { Remove-Item $pagesRoot -Recurse -Force }
New-Item -ItemType Directory -Path $pagesRoot | Out-Null

$utf8 = New-Object System.Text.UTF8Encoding($false)
$pageGuids = @()
$cIndex = 0
while ($cIndex -lt $allChurches.Count) {
	$pageChurches = $allChurches[$cIndex..([Math]::Min($cIndex + $churchesPerPage, $allChurches.Count) - 1)]
	# Build this page's button list: organ first, then churches, then fillers.
	$slice = @()
	$slice += @{ type = 'organ' }
	foreach ($c in $pageChurches) { $slice += @{ type = 'ir'; church = $c } }
	while ($slice.Count -lt $SlotsPerPage) { $slice += @{ type = 'filler' } }

	$actions = [ordered]@{}
	for ($s = 0; $s -lt $slice.Count; $s++) {
		$col = $s % $Cols
		$row = [Math]::Floor($s / $Cols)
		$key = "$col,$row"
		$b = $slice[$s]
		if ($b.type -eq 'organ')      { $actions[$key] = (New-CurrentOrganAction) }
		elseif ($b.type -eq 'filler') { $actions[$key] = (New-FillerAction) }
		else                          { $actions[$key] = (New-IrAction $b.church) }
	}
	$pageManifest = [ordered]@{
		Controllers = @([ordered]@{ Actions = $actions; Type = 'Keypad' })
		Icon = ''
		Name = ''
	}
	$guid = [guid]::NewGuid().ToString().ToUpper()
	$pageDir = Join-Path $pagesRoot $guid
	New-Item -ItemType Directory -Path $pageDir | Out-Null
	[System.IO.File]::WriteAllText((Join-Path $pageDir 'manifest.json'),
		($pageManifest | ConvertTo-Json -Depth 20 -Compress), $utf8)
	$pageGuids += $guid
	$cIndex += $churchesPerPage
}

Write-Host "Generated $($pageGuids.Count) page(s) for $($churches.Count) churches + Current Organ on every page"

# Create an EMPTY landing page used as Default, exactly like the known-working
# profile: Stream Deck Mobile shows the pages listed in Pages[] as the
# swipeable pages, while Default is a separate base page.
$emptyManifest = [ordered]@{
	Controllers = @([ordered]@{ Actions = [ordered]@{}; Type = 'Keypad' })
	Icon = ''
	Name = ''
}
$emptyGuid = [guid]::NewGuid().ToString().ToUpper()
$emptyDir = Join-Path $pagesRoot $emptyGuid
New-Item -ItemType Directory -Path $emptyDir | Out-Null
[System.IO.File]::WriteAllText((Join-Path $emptyDir 'manifest.json'),
	($emptyManifest | ConvertTo-Json -Depth 20 -Compress), $utf8)

# --- Update the profile manifest with the new page list ------------------
# Match the structure of a known-working Stream Deck Mobile profile:
#  - Current is the all-zero GUID (tells Stream Deck to start at Default).
#  - Default points to the EMPTY landing page.
#  - Pages[] holds ALL content pages (this is what Stream Deck shows/swipes).
#  - Page GUIDs are stored lowercase in the manifest even though the page
#    directories on disk are uppercase (Stream Deck Mobile expects this).
$profileManifest = Get-Content $profileManifestPath -Raw | ConvertFrom-Json
$default = $emptyGuid.ToLower()
$content = $pageGuids | ForEach-Object { $_.ToLower() }
$profileManifest.Pages = [ordered]@{
	Current = '00000000-0000-0000-0000-000000000000'
	Default = $default
	Pages   = @($content)
}
# Use the real Stream Deck Mobile device UUID so Stream Deck recognises the
# profile as belonging to this device instead of re-importing/merging it.
if ($profileManifest.Device) {
	$profileManifest.Device.UUID = 'ab512629-ddad-4c65-83d2-fba5217aeb63'
	$profileManifest.Device.Model = 'VSD2/WiFi'
}
[System.IO.File]::WriteAllText($profileManifestPath,
	($profileManifest | ConvertTo-Json -Depth 20 -Compress), $utf8)

# --- Repackage (root entries: package.json + Profiles/...) ---------------
Add-Type -AssemblyName System.IO.Compression.FileSystem
$outZip = Join-Path $stage 'out.zip'
if (Test-Path $outZip) { Remove-Item $outZip -Force }
[System.IO.Compression.ZipFile]::CreateFromDirectory($x, $outZip, [System.IO.Compression.CompressionLevel]::Optimal, $false)

Copy-Item $outZip $ProfilePath -Force
Write-Host "Rebuilt profile package: $ProfilePath"

# Also refresh the installed plugin copy if present.
$installed = Join-Path $env:APPDATA 'Elgato\StreamDeck\Plugins\com.ahlbornbridge.organ.sdPlugin\OrganLoaded.streamDeckProfile'
if (Test-Path (Split-Path $installed)) {
	Copy-Item $ProfilePath $installed -Force
	Write-Host "Updated installed profile: $installed"
}

Remove-Item $stage -Recurse -Force
