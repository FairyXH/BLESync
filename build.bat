@echo off
setlocal
cd /d "%~dp0"
if not exist build mkdir build
if not exist build\BLESync.exe (
  where g++ >nul 2>&1 || (
    echo g++ not found. Install MinGW-w64 and add it to PATH.
    exit /b 1
  )
  g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -DUNICODE -D_UNICODE -municode -Isrc src\main.cpp -o build\BLESync.exe -ladvapi32 -lsetupapi -lcfgmgr32 -lbthprops -lversion > build\compile.log 2>&1
  if errorlevel 1 (
    type build\compile.log
    exit /b 1
  )
)
copy /y BLESync.ini build\BLESync.ini >nul
if errorlevel 1 exit /b 1
echo Build succeeded: %~dp0build\BLESync.exe
