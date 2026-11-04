# Patches the IMPORTED OrganLoaded profile(s) that Stream Deck actually renders.
# Stream Deck keeps its own copy under %APPDATA%\Elgato\StreamDeck\ProfilesV3,
# independent from the bundled .streamDeckProfile. Editing the bundle does NOT
# affect an already-imported profile, so we patch the imported copy directly.
#
# Layout written across multiple pages:
#   page 1 slot 0,0 : Current Organ (currentOrgan=true)
#   remaining slots  : one com.ahlbornbridge.ir button per church (from churches.json)
# Grid is 3 columns x 2 rows (6 slots/page) for the Stream Deck Mobile.

$ErrorActionPreference = 'Stop'

$root = Join-Path $env:APPDATA 'Elgato\StreamDeck\ProfilesV3'
$ChurchesJson = Join-Path $PSScriptRoot 'churches.json'
if (-not (Test-Path $ChurchesJson)) { throw "churches.json not found: $ChurchesJson" }
$churches = Get-Content $ChurchesJson -Raw | ConvertFrom-Json
# Show churches in alphabetical order by name.
$churches = $churches | Sort-Object { $_.name }

$Cols = 5; $Rows = 3; $SlotsPerPage = $Cols * $Rows

# Delete any duplicate "OrganLoaded copy*" profiles that Stream Deck creates when
# a profile with the same name is re-imported. They confuse switchToProfile and
# can open on the wrong page. Keep only the canonical "OrganLoaded".
Get-ChildItem $root -Directory -Filter '*.sdProfile' | ForEach-Object {
	$m = Join-Path $_.FullName 'manifest.json'
	if (Test-Path $m) {
		$name = (Get-Content $m -Raw | ConvertFrom-Json).Name
		if ($name -like 'OrganLoaded *' -or $name -like 'OrganLoaded copy*') {
			Write-Host "Removing duplicate profile '$name' ($($_.Name))"
			Remove-Item $_.FullName -Recurse -Force
		}
	}
}

# Find the canonical imported OrganLoaded profile.
$targets = Get-ChildItem $root -Directory -Filter '*.sdProfile' | Where-Object {
	$m = Join-Path $_.FullName 'manifest.json'
	(Test-Path $m) -and ((Get-Content $m -Raw | ConvertFrom-Json).Name -eq 'OrganLoaded')
}
if (-not $targets) { throw 'No imported OrganLoaded profile found in ProfilesV3' }

$plugin = [ordered]@{ Name = 'AhlbornBridge'; UUID = 'com.ahlbornbridge.organ'; Version = '1.0.389' }

function New-State {
	param([string]$alignment = 'middle', [bool]$showTitle = $true)
	[ordered]@{ FontFamily=''; FontSize=11; FontStyle=''; FontUnderline=$false; OutlineThickness=2; ShowTitle=$showTitle; TitleAlignment=$alignment; TitleColor='#ffffff' }
}

function New-OrganButton {
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

function New-IrButton {
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
		States      = @((New-State 'bottom' $false))
		UUID        = 'com.ahlbornbridge.ir'
	}
}

function New-FillerButton {
	# An empty IR button that paints itself black, preventing ghost images from
	# other pages showing through unused slots on the Stream Deck Mobile.
	[ordered]@{
		ActionID    = [guid]::NewGuid().ToString()
		LinkedTitle = $true
		Name        = ''
		Plugin      = $plugin
		Resources   = $null
		Settings    = [ordered]@{ churchName = ''; thumbnail = ''; oscFront = ''; oscSide = ''; oscRear = '' }
		State       = 0
		States      = @((New-State 'bottom' $false))
		UUID        = 'com.ahlbornbridge.ir'
	}
}

# Ordered button list: Current Organ first, then all churches.
$buttons = @()
$buttons += @{ type = 'organ' }
foreach ($c in $churches) { $buttons += @{ type = 'ir'; church = $c } }

# Pad the final page with filler buttons so no slot is left empty (ghosting).
$remainder = $buttons.Count % $SlotsPerPage
if ($remainder -ne 0) {
	for ($i = 0; $i -lt ($SlotsPerPage - $remainder); $i++) { $buttons += @{ type = 'filler' } }
}

function Build-PageJson {
	param($slice)
	$actions = [ordered]@{}
	for ($s = 0; $s -lt $slice.Count; $s++) {
		$col = $s % $Cols
		$row = [Math]::Floor($s / $Cols)
		$key = "$col,$row"
		$b = $slice[$s]
		if ($b.type -eq 'organ')      { $actions[$key] = (New-OrganButton) }
		elseif ($b.type -eq 'filler') { $actions[$key] = (New-FillerButton) }
		else                          { $actions[$key] = (New-IrButton $b.church) }
	}
	$page = [ordered]@{
		Controllers = @([ordered]@{ Actions = $actions; Type = 'Keypad' })
		Icon = ''
		Name = ''
	}
	$page | ConvertTo-Json -Depth 20 -Compress
}

$utf8 = New-Object System.Text.UTF8Encoding($false)

foreach ($t in $targets) {
	$manifestPath = Join-Path $t.FullName 'manifest.json'
	$manifest = Get-Content $manifestPath -Raw | ConvertFrom-Json
	$pagesDir = Join-Path $t.FullName 'Profiles'

	# Remove existing page folders and rebuild them from scratch.
	Get-ChildItem $pagesDir -Directory | Remove-Item -Recurse -Force

	$pageGuids = @()
	$index = 0
	while ($index -lt $buttons.Count) {
		$slice = $buttons[$index..([Math]::Min($index + $SlotsPerPage, $buttons.Count) - 1)]
		$guid = [guid]::NewGuid().ToString().ToUpper()
		$pageDir = Join-Path $pagesDir $guid
		New-Item -ItemType Directory -Path $pageDir | Out-Null
		New-Item -ItemType Directory -Path (Join-Path $pageDir 'Images') | Out-Null
		[System.IO.File]::WriteAllText((Join-Path $pageDir 'manifest.json'), (Build-PageJson $slice), $utf8)
		$pageGuids += $guid
		$index += $SlotsPerPage
	}

	# Point the profile manifest at the new default/current page and extra pages.
	# Current must be the all-zero GUID (meaning "open on Default") exactly like
	# the Stream Deck app writes when exporting a profile; otherwise it opens on
	# whatever page was last viewed.
	$default = $pageGuids[0]
	$extra = @()
	if ($pageGuids.Count -gt 1) { $extra = $pageGuids[1..($pageGuids.Count - 1)] }
	$manifest.Pages = [ordered]@{
		Current = '00000000-0000-0000-0000-000000000000'
		Default = $default
		Pages   = @($extra)
	}
	[System.IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 20 -Compress), $utf8)
	Write-Host "Patched $($t.Name): $($pageGuids.Count) page(s)"
}

Write-Host 'Done. Restart Stream Deck for changes to appear.'
