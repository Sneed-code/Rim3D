<#
.SYNOPSIS
  Packages Rim3D (Development, Win64) at low priority, for performance checks in a real build (task G3).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\package.ps1
  powershell -ExecutionPolicy Bypass -File tools\smoke.ps1 -Test perf -Packaged -Force

.NOTES
  Output: Rim3D-packaged\Windows\Rim3D.exe next to the repo (-PackageDir to change). The game reads its defs
  from the repo's data folder (-SimData=, which smoke.ps1 -Packaged passes), so data changes need no repackage.
  Everything runs below normal priority on the first $Threads cores (compilers, the cooker and the shader
  workers inherit both), so the PC stays usable. Log: %TEMP%\rim3d-package.log.
  -SkipCook packages new code over the last cook (C++ changes only).
  Exit code: 0 packaged, 2 failed.
#>
param(
    [int]$Threads = 6,
    [string]$PackageDir = "",
    [switch]$SkipCook,
    [string]$EngineDir = "C:\Program Files\Epic Games\UE_5.8"
)

$ErrorActionPreference = "Continue"
$Repo = Split-Path -Parent $PSScriptRoot
$Project = Join-Path $Repo "game\Rim3D.uproject"
$Uat = Join-Path $EngineDir "Engine\Build\BatchFiles\RunUAT.bat"
if (-not $PackageDir) { $PackageDir = Join-Path (Split-Path -Parent $Repo) "Rim3D-packaged" }
$log = Join-Path $env:TEMP "rim3d-package.log"

foreach ($path in @($Project, $Uat)) {
    if (-not (Test-Path $path)) { Write-Host "Missing: $path"; exit 2 }
}

$cook = if ($SkipCook) { "-skipcook" } else { "-cook" }
$uatArgs = "BuildCookRun -project=`"$Project`" -noP4 -platform=Win64 -clientconfig=Development -build $cook -stage -pak -archive " +
    "-archivedirectory=`"$PackageDir`" -unattended -utf8output -ubtargs=`"-MaxParallelActions=$Threads`""
Write-Host "Packaging Rim3D (Development, below-normal priority, $Threads cores) into $PackageDir ..."
# `start /belownormal /affinity` sets both before UAT runs, so every process it starts inherits them. UAT goes in a
# small batch file run by `cmd /c` (start mangles a quoted program path followed by quoted arguments, and runs a
# batch file with cmd /K, which never exits).
$mask = "{0:X}" -f ([int64][math]::Pow(2, $Threads) - 1)
$wrapper = Join-Path $env:TEMP "rim3d-package.bat"
Set-Content -Path $wrapper -Encoding ASCII -Value "@call `"$Uat`" $uatArgs > `"$log`" 2>&1"
$p = Start-Process -FilePath "cmd.exe" -ArgumentList "/c start `"`" /b /wait /belownormal /affinity $mask cmd /c `"$wrapper`"" -PassThru -WindowStyle Hidden
$p.WaitForExit()
if ($p.ExitCode -ne 0 -or -not (Test-Path (Join-Path $PackageDir "Windows\Rim3D.exe"))) {
    Write-Host "Packaging failed (exit $($p.ExitCode)). Last lines of $log :"
    Get-Content $log -Tail 30
    exit 2
}
Write-Host "Packaged: $(Join-Path $PackageDir 'Windows\Rim3D.exe')"
exit 0
