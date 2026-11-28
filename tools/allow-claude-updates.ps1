# (Copy of the script run once on sneed's PC.) Lets Claude update and test the game in C:\Dev\Rim3D without asking you to type OK.
# Adds narrow allow rules and a note for the auto mode safety check to YOUR Claude Code settings
# (%USERPROFILE%\.claude\settings.json). Keeps everything already there and saves a backup first.
$ErrorActionPreference = 'Stop'
$dir  = Join-Path $env:USERPROFILE '.claude'
$file = Join-Path $dir 'settings.json'
if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
if (Test-Path $file) {
    Copy-Item $file "$file.backup-$(Get-Date -Format yyyyMMdd-HHmmss)"
    $text = [IO.File]::ReadAllText($file)
    if ($text.Trim().Length -eq 0) { $cfg = [pscustomobject]@{} } else { $cfg = $text | ConvertFrom-Json }
} else { $cfg = [pscustomobject]@{} }
function Ensure($obj, $name, $value) {
    if (-not ($obj.PSObject.Properties.Name -contains $name)) { $obj | Add-Member -NotePropertyName $name -NotePropertyValue $value }
}
Ensure $cfg 'permissions' ([pscustomobject]@{})
Ensure $cfg.permissions 'allow' @()
$allow = @($cfg.permissions.allow)
$rules = @(
  'Bash(git pull:*)', 'Bash(git -C C:/Dev/Rim3D pull:*)', 'Bash(git fetch:*)',
  'Bash(python tools/update.py:*)', 'Bash(python tools\update.py:*)', 'Bash(py tools/update.py:*)',
  'Bash(python C:/Dev/Rim3D/tools/update.py:*)', 'Bash(cmd /c tools\update-and-play.bat:*)',
  'PowerShell(git pull:*)', 'PowerShell(git fetch:*)',
  'PowerShell(python tools/update.py:*)', 'PowerShell(python tools\update.py:*)', 'PowerShell(py tools\update.py:*)',
  'PowerShell(python C:\Dev\Rim3D\tools\update.py:*)', 'PowerShell(& .\tools\smoke.ps1:*)',
  'PowerShell(.\tools\update-and-play.bat:*)', 'PowerShell(cmd /c tools\update-and-play.bat:*)'
)
foreach ($r in $rules) { if ($allow -notcontains $r) { $allow += $r } }
$cfg.permissions.allow = $allow
Ensure $cfg 'autoMode' ([pscustomobject]@{})
Ensure $cfg.autoMode 'environment' @('$defaults')
$envList = @($cfg.autoMode.environment)
if ($envList -notcontains '$defaults') { $envList = @('$defaults') + $envList }
$notes = @(
  'C:\Dev\Rim3D is the owner''s own private game project (repo Sneed-code/Rim3D). The owner has given standing approval: git pull/fetch there, running tools\update.py and tools\update-and-play.bat, the art steps and Unreal commandlets they start, building and launching the game, and smoke tests (tools\smoke.ps1) are routine and approved without asking.',
  'Test copies under C:\Dev\Rim3D-* are the owner''s own test clones; building, copying Content into them and running the game there is routine and approved.'
)
foreach ($n in $notes) { if ($envList -notcontains $n) { $envList += $n } }
$cfg.autoMode.environment = $envList
$json = $cfg | ConvertTo-Json -Depth 32
[IO.File]::WriteAllText($file, $json, (New-Object System.Text.UTF8Encoding($false)))
Write-Host "Done. Claude can now update C:\Dev\Rim3D without asking. Settings: $file"
