@echo off
rem Opens the Tauri window from this checkout, against the C++ launcher built in flash-launcher-multiplayer and your
rem own launcher.json and settings. Closing this console window closes the launcher too.
cd /d "%~dp0"
set "FL_LAUNCHER_EXE=%~dp0..\..\flash-launcher-multiplayer\build\Release\MultiplayerEverythingLauncher.exe"
if not exist "%FL_LAUNCHER_EXE%" (
  echo The C++ launcher isn't built yet: %FL_LAUNCHER_EXE%
  pause
  exit /b 1
)
npx tauri dev
