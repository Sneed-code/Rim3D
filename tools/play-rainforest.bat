@echo off
rem Starts the packaged playtest build (G2) on a tropical rainforest map: the Crashlanded start on a
rem rainforest tile of world 42. Package first with tools\package.ps1. Extra arguments are passed on,
rem e.g. play-rainforest.bat -SimSeed=7 for another world and map. Steps: docs\setup\playtest.md.
set "REPO=%~dp0.."
set "GAME=%REPO%\..\Rim3D-packaged\Windows\Rim3D.exe"
if not exist "%GAME%" (
  echo The game is not packaged yet. Run: powershell -ExecutionPolicy Bypass -File "%REPO%\tools\package.ps1"
  pause
  exit /b 2
)
start "" "%GAME%" -SimData="%REPO%\data" -SimWorld -SimScenario=CrashlandedRainforest -SimOriginX=300000 -Overseer %*
