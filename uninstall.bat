@echo off
setlocal
cd /d "%~dp0"
net session >nul 2>&1
if errorlevel 1 (
  echo Administrator privileges are required.
  exit /b 1
)
if exist build\BLESync.exe build\BLESync.exe --uninstall
