@echo off
setlocal EnableExtensions DisableDelayedExpansion
cd /d "%~dp0"
if not exist build mkdir build
where g++ >nul 2>&1 || (
  echo ERROR: g++ not found. Install MinGW-w64 and add it to PATH.
  exit /b 1
)
echo Building BLESync...
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -DUNICODE -D_UNICODE -municode -Isrc src\main.cpp src\wifi.cpp -o build\BLESync.exe -static-libgcc -static-libstdc++ -lsetupapi -lcfgmgr32 -ladvapi32 -lbcrypt -lshell32 -lole32 -lwlanapi > build\compile.log 2>&1
if errorlevel 1 (
  type build\compile.log
  echo Build failed.
  exit /b 1
)
copy /y BLESync.ini build\BLESync.ini >nul
if errorlevel 1 exit /b 1
echo Build succeeded: %~dp0build\BLESync.exe
