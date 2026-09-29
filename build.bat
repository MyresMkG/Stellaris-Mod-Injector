@echo off
rem Build the five proxy-DLL variants of the injected_mods loader.
rem
rem Every variant takes the name of a system DLL the game loads anyway and
rem forwards all of its exports to the real file in System32 (def\<name>.def),
rem so the game cannot tell the difference -- except that DllMain now loads
rem every DLL from injected_mods once the game's own image is up.
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
set SRC=%ROOT%src
set DEF=%ROOT%def
rem The five DLLs land in ..\stellaris_mod_injector_dll\ -- the folder players
rem copy from. Set OUTDIR to build somewhere else; tools\run_tests.py does that
rem so that running the tests never overwrites the DLLs verified in game.
set OUT=%OUTDIR%
if "%OUT%"=="" set OUT=%ROOT%..\stellaris_mod_injector_dll
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%ROOT%build" mkdir "%ROOT%build"

set COMMON=-std=c++17 -O2 -Wall -Wextra -static -static-libgcc -static-libstdc++
set SOURCES=%SRC%\dllmain.cpp %SRC%\crtprobe.cpp %SRC%\mods.cpp %SRC%\config.cpp %SRC%\log.cpp %SRC%\util.cpp

for %%N in (dxgi d3d11 d3d9 version winmm) do (
  echo building %%N.dll
  g++ %COMMON% -shared -o "%OUT%\%%N.dll" %SOURCES% "%DEF%\%%N.def" -lkernel32 -Wl,--out-implib,"%ROOT%build\lib%%N.a"
  if errorlevel 1 exit /b 1
)

echo.
echo done:
dir /b "%OUT%"
endlocal
