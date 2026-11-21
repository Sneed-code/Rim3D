@echo off
rem The game on its title screen: Continue, New colony, Load, Settings, Debug (the gun range, the character creator and
rem the other test scenes) and Quit. Runs this copy as it is; tools\update-and-play.bat updates it first.
rem Extra arguments are passed on, e.g. play.bat -ResX=2560 -ResY=1440.
set "REPO=%~dp0.."
set "EDITOR=C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe"
start "" "%EDITOR%" "%REPO%\game\Rim3D.uproject" /Game/Levels/DefaultLevel -game -windowed -ResX=1920 -ResY=1080 -nosplash %*
