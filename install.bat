@echo off
setlocal
cd /d "%~dp0"
if not exist build\BLESync.exe (
  echo build\BLESync.exe not found. Run build.bat first.
  exit /b 1
)
net session >nul 2>&1
if errorlevel 1 (
  echo Administrator privileges are required.
  exit /b 1
)
build\BLESync.exe --install
