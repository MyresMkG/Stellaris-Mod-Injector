@echo off
rem Build stellaris_mod_injector.exe (the injected_mods loader) with MinGW-w64.
rem
rem Source and compiled program are kept apart on purpose: the exe lands in
rem ..\stellaris_mod_injector_bin\. Pass another output directory as the first
rem argument to put it somewhere else.
rem
rem Set MINGW_BIN if g++ is not on PATH, e.g.
rem   set MINGW_BIN=D:\Program Files\mingw64\bin
setlocal

if not "%MINGW_BIN%"=="" set "PATH=%MINGW_BIN%;%PATH%"

where g++ >nul 2>nul
if errorlevel 1 (
  echo g++ not found. Install MinGW-w64 and add its bin directory to PATH,
  echo or set MINGW_BIN to it.
  exit /b 1
)

set ROOT=%~dp0
set OUT=%~1
if "%OUT%"=="" set OUT=%ROOT%..\stellaris_mod_injector_bin

if not exist "%OUT%" mkdir "%OUT%"

set COMMON=-std=c++17 -O2 -Wall -Wextra -static -static-libgcc -static-libstdc++

echo building stellaris_mod_injector.exe
g++ %COMMON% -o "%OUT%\stellaris_mod_injector.exe" "%ROOT%stellaris_mod_injector.cpp" -lkernel32
if errorlevel 1 exit /b 1

echo.
echo done:
dir /b "%OUT%\stellaris_mod_injector.exe"
endlocal
