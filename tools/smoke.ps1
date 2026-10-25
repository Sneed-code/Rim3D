<#
.SYNOPSIS
  Builds the Rim3D game at low priority, runs one smoke test, and reports the result.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\smoke.ps1 -Test load

.NOTES
  Tests: load, crowd, terrain, sim (see game/Source/Rim3D/SmokeTest.h).
  Output: game\Saved\Smoke\<test>.json and game\Saved\Smoke\<test>\*.png
  Exit code: 0 pass, 1 test failed, 2 build failed or PC busy, 3 crashed or timed out.
  Only run this while the PC is free: no game and no local AI tool (ComfyUI and the like).
#>
param(
    [Parameter(Mandatory = $true)][string]$Test,
    [switch]$NoBuild,
    [switch]$BuildOnly,                   # compile, but do not start the game
    [switch]$Visible,                     # show the game window (default: draw offscreen, no pop-up)
    [switch]$Force,                       # skip the "is the PC busy" check
    [string]$Resolution = "1920x1080",
    [int]$Threads = 6,                    # cap for compiler jobs
    [int]$TimeoutSeconds = 300,           # the test fails itself after this long
    [string]$EngineDir = "C:\Program Files\Epic Games\UE_5.8",
    [string[]]$ExtraArgs = @()           # more game arguments, e.g. "-ini:Engine:[SystemSettings]:Terrain.Textured=0" (a console variable from the first frame)
)

$ErrorActionPreference = "Continue"
(Get-Process -Id $PID).PriorityClass = "BelowNormal"   # compilers and the game inherit this
$Repo = Split-Path -Parent $PSScriptRoot
$Project = Join-Path $Repo "game\Rim3D.uproject"
$SmokeDir = Join-Path $Repo "game\Saved\Smoke"
$Editor = Join-Path $EngineDir "Engine\Binaries\Win64\UnrealEditor.exe"
$BuildBat = Join-Path $EngineDir "Engine\Build\BatchFiles\Build.bat"

foreach ($path in @($Project, $Editor, $BuildBat)) {
    if (-not (Test-Path $path)) { Write-Host "Missing: $path"; exit 2 }
}

# --- Is the PC free? -------------------------------------------------------
if (-not $Force -and -not $BuildOnly) {
    $busy = @()
    if (Get-Process -Name "UnrealEditor", "UnrealEditor-Cmd" -ErrorAction SilentlyContinue) { $busy += "Unreal Editor is already running" }
    $smi = Get-Command nvidia-smi -ErrorAction SilentlyContinue
    if ($smi) {
        $gpu = (& nvidia-smi --query-gpu=memory.used,utilization.gpu --format=csv,noheader,nounits) -split ","
        $usedMb = [int]$gpu[0].Trim()
        $util = [int]$gpu[1].Trim()
        if ($usedMb -gt 3000 -or $util -gt 30) { $busy += "the graphics card is in use ($usedMb MB, $util% busy)" }
    }
    if ($busy.Count -gt 0) {
        Write-Host "PC looks busy: $($busy -join '; '). Close that, or rerun with -Force."
        exit 2
    }
}

# --- Build at low priority --------------------------------------------------
if (-not $NoBuild) {
    Write-Host "Building Rim3DEditor (below-normal priority, $Threads jobs)..."
    $buildLog = Join-Path $env:TEMP "rim3d-build.log"
    & $BuildBat Rim3DEditor Win64 Development "-Project=$Project" "-MaxParallelActions=$Threads" -WaitMutex *> $buildLog
    $buildExit = $LASTEXITCODE
    if ($buildExit -ne 0) {
        Write-Host "Build failed (exit $buildExit). Last lines of $buildLog :"
        Get-Content $buildLog -Tail 25
        exit 2
    }
    Write-Host "Build ok."
    if ($BuildOnly) { exit 0 }
}

# --- Run the test -----------------------------------------------------------
Remove-Item (Join-Path $SmokeDir "$Test.json") -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $SmokeDir $Test) -Recurse -Force -ErrorAction SilentlyContinue

$res = $Resolution -split "x"
$gameArgs = @(
    "`"$Project`"", "/Game/Levels/DefaultLevel", "-game", "-windowed", "-ResX=$($res[0])", "-ResY=$($res[1])",
    "-nosplash", "-nosound", "-unattended", "-SmokeTest=$Test", "-SmokeTimeout=$TimeoutSeconds"
)
if (-not $Visible) { $gameArgs += "-RenderOffScreen" }
$gameArgs += $ExtraArgs
Write-Host "Running smoke test '$Test' at $Resolution..."
$game = Start-Process -FilePath $Editor -ArgumentList $gameArgs -PassThru
try { $game.PriorityClass = "BelowNormal" } catch { }
if (-not $game.WaitForExit(($TimeoutSeconds + 120) * 1000)) {
    Write-Host "The game did not quit in time; stopping it."
    $game.Kill()
    $game.WaitForExit()
    $timedOut = $true
}

# --- Report -----------------------------------------------------------------
$jsonPath = Join-Path $SmokeDir "$Test.json"
if (-not (Test-Path $jsonPath)) {
    Write-Host "CRASH or no result: $jsonPath was not written (exit code $($game.ExitCode))."
    $log = Join-Path $Repo "game\Saved\Logs\Rim3D.log"
    if (Test-Path $log) { Write-Host "Last log lines:"; Get-Content $log -Tail 20 }
    exit 3
}

$r = Get-Content $jsonPath -Raw | ConvertFrom-Json
Write-Host ""
Write-Host "Smoke test '$($r.test)': $($r.status.ToUpper())  ($([math]::Round($r.durationSeconds)) s, $($r.gpu), $($r.resolution))"
foreach ($p in $r.phases) {
    $shot = Join-Path $SmokeDir $p.screenshot
    $shotOk = if (Test-Path $shot) { "screenshot ok" } else { "NO SCREENSHOT" }
    "{0,-26} {1,6:N1} fps  game {2,5:N1} ms  render {3,5:N1} ms  gpu {4,5:N1} ms  {5}" -f $p.phase, $p.avgFps, $p.medianGameThreadMs, $p.medianRenderThreadMs, $p.medianGpuMs, $shotOk
    if (-not (Test-Path $shot)) { $missingShot = $true }
}
foreach ($n in $r.notes) { Write-Host "note: $n" }
foreach ($e in $r.errors) { Write-Host "ERROR: $e" }
Write-Host "Screenshots: $(Join-Path $SmokeDir $Test)"

if ($r.status -ne "pass" -or $missingShot) { exit 1 }
exit 0



