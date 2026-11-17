@echo off
rem The gun range (docs/design/gun-range.md): first person with real gun models, random creator women to shoot at.
rem Keys: WASD walk, mouse look, left mouse fire, right mouse aim down the sights, R reload, 1-4 or the mouse wheel pick a
rem gun, N new targets, Shift sprint, F2 pause, Esc quit. H free hand (docs/design/free-hand.md): a partner stands at
rem arm's length; the mouse moves the hand, hold left click to grip, wheel reach, hold right mouse to look. Ammunition is unlimited; magazines still need reloading.
rem Needs the editor build (tools\smoke.ps1 -Test load -BuildOnly), the creator bases and clips (MakeHumanArt) and the
rem gun models (GunArt commandlet). Extra arguments are passed on, e.g. -GunRangeSeed=7 for other women.
set "REPO=%~dp0.."
set "EDITOR=C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe"
start "" "%EDITOR%" "%REPO%\game\Rim3D.uproject" /Game/Levels/DefaultLevel -game -windowed -ResX=1920 -ResY=1080 -nosplash -SmokeTest=gunrange -SmokeTimeout=14700 %*
