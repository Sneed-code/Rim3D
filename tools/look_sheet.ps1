<#
.SYNOPSIS
  The look bench's before and after sheet (task L1, docs/design/look.md).

.DESCRIPTION
  Copies the `look` smoke test's shots into Saved\Look\<date and time>[_label]\ and makes sheet.png there: one row per
  shot, the last run on the left and this run on the right. smoke.ps1 runs it after the look test; it can also be run by
  hand to compare any two runs.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\look_sheet.ps1 -Shots game\Saved\Smoke\look -LookDir game\Saved\Look -Label after
  powershell -ExecutionPolicy Bypass -File tools\look_sheet.ps1 -LookDir game\Saved\Look -Run 2026-10-28_0130_after -Against 2026-10-28_0040_before
#>
param(
    [string]$Shots = "",       # the smoke test's screenshots (copied into a new run folder); empty: use -Run as it is
    [Parameter(Mandatory = $true)][string]$LookDir,
    [string]$Label = "",
    [string]$Run = "",         # an existing run folder's name to make the sheet in (instead of copying -Shots)
    [string]$Against = "",     # the run to compare with (default: the newest older run)
    [int]$CellWidth = 800
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Force $LookDir | Out-Null

if ($Shots) {
    $name = (Get-Date -Format "yyyy-MM-dd_HHmm") + $(if ($Label) { "_$Label" } else { "" })
    $runDir = Join-Path $LookDir $name
    New-Item -ItemType Directory -Force $runDir | Out-Null
    Get-ChildItem $Shots -Filter *.png | Where-Object { -not $_.Name.StartsWith("_") } | Copy-Item -Destination $runDir -Force
} elseif ($Run) {
    $runDir = Join-Path $LookDir $Run
} else {
    Write-Host "Give -Shots or -Run."
    exit 1
}
$runName = Split-Path -Leaf $runDir

if ($Against) {
    $againstDir = Join-Path $LookDir $Against
} else {
    $older = Get-ChildItem $LookDir -Directory | Where-Object { $_.Name -lt $runName } | Sort-Object Name | Select-Object -Last 1
    $againstDir = if ($older) { $older.FullName } else { $null }
}
$againstName = if ($againstDir) { Split-Path -Leaf $againstDir } else { "(no earlier run)" }

# The shots in the bench's own order, then any others by name.
$order = @("overseer_high", "overseer_low", "colonist_closeup", "room_lit", "dusk", "rainy_night", "hud", "walls", "snowy_day")
$names = @(Get-ChildItem $runDir -Filter *.png | Where-Object { $_.Name -ne "sheet.png" } | ForEach-Object { $_.BaseName })
$rows = @($order | Where-Object { $names -contains $_ }) + @($names | Where-Object { $order -notcontains $_ } | Sort-Object)
if ($rows.Count -eq 0) { Write-Host "No shots in $runDir"; exit 1 }

$cellH = [int]($CellWidth * 9 / 16)
$labelH = 34
$headH = 44
$width = $CellWidth * 2 + 12
$height = $headH + $rows.Count * ($cellH + $labelH)
$sheet = New-Object System.Drawing.Bitmap $width, $height
$g = [System.Drawing.Graphics]::FromImage($sheet)
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$g.Clear([System.Drawing.Color]::FromArgb(24, 24, 26))
$font = New-Object System.Drawing.Font "Segoe UI", 15
$bold = New-Object System.Drawing.Font "Segoe UI", 17, ([System.Drawing.FontStyle]::Bold)
$white = [System.Drawing.Brushes]::WhiteSmoke
$grey = [System.Drawing.Brushes]::Gray
$g.DrawString("Before: $againstName", $bold, $white, 8, 8)
$g.DrawString("After: $runName", $bold, $white, $CellWidth + 20, 8)

$y = $headH
foreach ($row in $rows) {
    $g.DrawString($row, $font, $white, 8, $y + 4)
    $cells = @(@{ Dir = $againstDir; X = 0 }, @{ Dir = $runDir; X = $CellWidth + 12 })
    foreach ($cell in $cells) {
        $path = if ($cell.Dir) { Join-Path $cell.Dir "$row.png" } else { $null }
        if ($path -and (Test-Path $path)) {
            $img = [System.Drawing.Image]::FromFile($path)
            # Fit 16:9, cropping the middle of other shapes.
            $srcW = $img.Width; $srcH = [int]($img.Width * 9 / 16)
            if ($srcH -gt $img.Height) { $srcH = $img.Height; $srcW = [int]($img.Height * 16 / 9) }
            $src = New-Object System.Drawing.Rectangle ([int](($img.Width - $srcW) / 2)), ([int](($img.Height - $srcH) / 2)), $srcW, $srcH
            $dst = New-Object System.Drawing.Rectangle $cell.X, ($y + $labelH), $CellWidth, $cellH
            $g.DrawImage($img, $dst, $src, [System.Drawing.GraphicsUnit]::Pixel)
            $img.Dispose()
        } else {
            $g.DrawString("no shot", $font, $grey, $cell.X + $CellWidth / 2 - 30, $y + $labelH + $cellH / 2)
        }
    }
    $y += $cellH + $labelH
}
$g.Dispose()
$out = Join-Path $runDir "sheet.png"
$sheet.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$sheet.Dispose()
Write-Host "Look sheet: $out (before: $againstName)"
