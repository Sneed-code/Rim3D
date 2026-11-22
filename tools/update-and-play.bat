@echo off
rem One click to play the newest version (docs/setup/update.md): pulls the newest main from GitHub, builds it at low
rem priority, redoes only the art steps whose inputs changed (hair, clothes, blood, sounds ...), then starts the game on
rem its title screen. The gun range, the character creator and the other test scenes are under Debug there.
rem Close the game before running this. The first run after a big update can take a while; later ones take minutes.
cd /d "%~dp0.."
python tools\update.py %*
if errorlevel 2 (
  echo.
  echo The update could not finish ^(see above^). Press a key to play the version you have, or close this window.
  pause >nul
) else if errorlevel 1 (
  echo.
  echo Some art steps failed ^(see above^); the game still runs without their newest look. Starting it in 10 seconds.
  timeout /t 10 >nul
)
call "%~dp0play.bat"
