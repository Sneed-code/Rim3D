@echo off
rem The character creator (docs/design/makehuman.md, "Character creator"): a woman, a man and an imp on the MakeHuman
rem creator bases with the graphics up and a panel of sliders (top left) that change the picked one as you drag.
rem "Walk back and forth" shows the jiggle and the tail. Needs the editor build (tools\smoke.ps1 -Test load -BuildOnly),
rem the bases (python tools\makehuman\mh_creator.py) and the MakeHumanArt commandlet. Extra arguments are passed on.
set "REPO=%~dp0.."
set "EDITOR=C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe"
start "" "%EDITOR%" "%REPO%\game\Rim3D.uproject" /Game/Levels/DefaultLevel -game -windowed -ResX=1920 -ResY=1080 -nosplash -SmokeTest=creator -SmokeTimeout=14700 %*
