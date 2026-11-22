@echo off
rem Also on the title screen under Debug (tools\play.bat). The combat range (docs/design/gun-range.md): first person with real gun and melee models, random creator women.
rem Keys: WASD walk, mouse look, left mouse fire / swing, right mouse aim / block, 1-0 or the mouse wheel pick a weapon,
rem K new targets, U guards, Tab focus, middle mouse lock on or let go, Shift sprint, T roll / dash, F2 pause, Esc quit. No reloads: guns cool down.
rem Needs the editor build (tools\smoke.ps1 -Test load -BuildOnly), the creator bases and clips (MakeHumanArt) and the
rem gun models (GunArt commandlet). Extra arguments are passed on, e.g. -GunRangeSeed=7 for other women.
set "REPO=%~dp0.."
set "EDITOR=C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe"
start "" "%EDITOR%" "%REPO%\game\Rim3D.uproject" /Game/Levels/DefaultLevel -game -windowed -ResX=1920 -ResY=1080 -nosplash -SmokeTest=gunrange -SmokeTimeout=14700 %*
