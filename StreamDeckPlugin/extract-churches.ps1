# Extracts Altiverb IR church data from Book.xlsx and emits churches.json.
#
# Table columns: ID | NAME | IR_Autom | DSP TYPE | Meters | Seconds
# Rules (per church ID):
#   front = IR_Autom of the FRONT row (every church has FRONT)
#   side  = IR_Autom of the SIDE row  if present, else REAR, else FRONT
#   rear  = IR_Autom of the REAR row  if present, else FRONT
# The matching thumbnail PNG is located by NAME inside the same folder.

$ErrorActionPreference = 'Stop'

$IrDir = 'C:\Users\paolo\source\repos\paoloantiga70\AhlbornBridge2\x64\Release\IR_thumbnails'
$Xlsx  = Join-Path $IrDir 'Book.xlsx'
$OutJson = Join-Path $PSScriptRoot 'churches.json'

if (-not (Test-Path $Xlsx)) { throw "Book.xlsx not found: $Xlsx" }

# --- Read the xlsx (zip of XML) without Excel ----------------------------
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead($Xlsx)
function Read-Entry($name) {
	$e = $zip.Entries | Where-Object { $_.FullName -eq $name }
	$sr = New-Object System.IO.StreamReader($e.Open())
	$t = $sr.ReadToEnd(); $sr.Close(); $t
}
$ssXml = Read-Entry 'xl/sharedStrings.xml'
$shared = [regex]::Matches($ssXml, '<t[^>]*>(.*?)</t>') | ForEach-Object { $_.Groups[1].Value }
[xml]$sheet = Read-Entry 'xl/worksheets/sheet1.xml'
$zip.Dispose()

function Get-CellValue($cell) {
	if ($null -eq $cell) { return $null }
	$v = $cell.v
	if ($cell.t -eq 's') { return [string]$shared[[int]$v] }
	return $v
}

# --- Parse rows into objects ---------------------------------------------
$rows = @()
foreach ($row in $sheet.worksheet.sheetData.row) {
	$map = @{}
	foreach ($c in $row.c) {
		$col = ($c.r -replace '\d','')   # e.g. "B12" -> "B"
		$map[$col] = Get-CellValue $c
	}
	if (-not $map.ContainsKey('A')) { continue }
	$id = $map['A']
	if ($id -eq 'ID' -or [string]::IsNullOrWhiteSpace($id)) { continue }  # skip header/empty
	$rows += [pscustomobject]@{
		ID      = [int]$id
		Name    = [string]$map['B']
		IRAutom = [string]$map['C']
		Type    = ([string]$map['D']).ToUpper()
	}
}

# --- Resolve thumbnail files ---------------------------------------------
$pngFiles = Get-ChildItem $IrDir -Filter *.png
function Find-Thumbnail($name) {
	$exact = $pngFiles | Where-Object { $_.BaseName -eq $name } | Select-Object -First 1
	if ($exact) { return $exact.Name }
	$starts = $pngFiles | Where-Object { $_.BaseName -like ($name + '*') } | Select-Object -First 1
	if ($starts) { return $starts.Name }
	$contains = $pngFiles | Where-Object { $_.BaseName -like ('*' + $name + '*') } | Select-Object -First 1
	if ($contains) { return $contains.Name }
	return ''
}

# --- Group by church ID ---------------------------------------------------
$churches = @()
foreach ($g in ($rows | Group-Object ID | Sort-Object { [int]$_.Name })) {
	$items = $g.Group
	$front = ($items | Where-Object { $_.Type -eq 'FRONT' } | Select-Object -First 1).IRAutom
	$side  = ($items | Where-Object { $_.Type -eq 'SIDE' }  | Select-Object -First 1).IRAutom
	$rear  = ($items | Where-Object { $_.Type -eq 'REAR' }  | Select-Object -First 1).IRAutom

	if (-not $front) {
		# No FRONT row: fall back to the first available position.
		$front = ($items | Select-Object -First 1).IRAutom
	}
	if (-not $rear)  { $rear = $front }
	if (-not $side)  { $side = $rear }

	$name = $items[0].Name
	$thumb = Find-Thumbnail $name
	if (-not $thumb) { Write-Warning "No thumbnail for '$name' (ID $($g.Name))" }

	$churches += [ordered]@{
		id        = [int]$g.Name
		name      = $name
		front     = $front
		side      = $side
		rear      = $rear
		thumbnail = $thumb
	}
}

$json = $churches | ConvertTo-Json -Depth 5
[System.IO.File]::WriteAllText($OutJson, $json, (New-Object System.Text.UTF8Encoding($false)))
Write-Host "Wrote $($churches.Count) churches to $OutJson"
$churches | ForEach-Object { "  [{0,2}] {1,-40} F={2} S={3} R={4}  thumb={5}" -f $_.id, $_.name, $_.front, $_.side, $_.rear, $_.thumbnail }
