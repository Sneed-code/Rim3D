@echo off
rem The MakeHuman showcase (docs/design/makehuman.md): the compare scene for play with the graphics turned up.
rem You control the ponytail woman (WASD, mouse to look, Ctrl walk, Space jump, F2 pause, V view, Tab overseer);
rem the long-haired woman stands as an NPC. Needs the editor build (tools\smoke.ps1 -Test load -BuildOnly) and the
rem MakeHuman assets (MakeHumanArt commandlet). Extra arguments are passed on, e.g. -ResX=2560 -ResY=1440.
set "REPO=%~dp0.."
set "EDITOR=C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe"
start "" "%EDITOR%" "%REPO%\game\Rim3D.uproject" /Game/Levels/DefaultLevel -game -windowed -ResX=1920 -ResY=1080 -nosplash -SmokeTest=showcase -SmokeTimeout=14700 %*
