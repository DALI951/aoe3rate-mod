@echo off
rem build.bat — AoE3 resource-rate mod build + verify + deploy (R14)
setlocal enabledelayedexpansion

set "W64=C:\Users\Dali\Documents\gaames\w64devkit-x86\w64devkit"
set "GCC=%W64%\bin\i686-w64-mingw32-gcc.exe"
set "PY=C:\Python314\python.exe"
set "GAME=C:\Users\Dali\Documents\gaames\Age of Empires III - Complete Collection"
set "ROOT=%~dp0.."

if not exist "%GCC%" (
  echo ERROR: gcc not found at %GCC%
  exit /b 1
)

rem 1) PATH prepend so cc1 is found
set "PATH=%W64%\bin;%W64%\libexec\gcc\i686-w64-mingw32\16.2.0;%PATH%"

pushd "%ROOT%"
echo [1/7] compiling ...
"%GCC%" -shared -static-libgcc -O2 -Wall -Wextra -o d3d9.dll d3d9.c d3d9.def -lwinmm -luser32 2>build_log.txt
set "RC=%ERRORLEVEL%"
type build_log.txt
if not "%RC%"=="0" (
  echo BUILD FAILED rc=%RC%
  popd
  exit /b 1
)
echo compiled rc=0

echo [2/7] verifying PE ...
"%PY%" "build\verify_pe.py" d3d9.dll
set "VRC=%ERRORLEVEL%"
if not "%VRC%"=="0" (
  echo VERIFY FAILED
  popd
  exit /b 1
)

echo [3/7] writing hash record
"%PY%" -c "import hashlib;print(hashlib.sha256(open('d3d9.dll','rb').read()).hexdigest())" > d3d9.sha256

echo [4/7] deploying to game dir + tests
copy /y d3d9.dll "%GAME%\d3d9.dll" >nul
copy /y d3d9.dll "%ROOT%\tests\d3d9.dll" >nul

echo [5/7] copying ini example if not present
if not exist "%GAME%\ResourceRateMod.ini" copy /y "ResourceRateMod.ini.example" "%GAME%\ResourceRateMod.ini" >nul

echo [6/7] shipping d3dx9_25.dll if absent
if exist "%GAME%\d3dx9_25.dll" (
  echo d3dx9_25.dll present in game dir
) else (
  echo d3dx9_25.dll NOT in game dir - scanning DX redist ...
  set "FOUND="
  for /r "%GAME%" %%F in (d3dx9_25.dll) do (
    if not exist "%GAME%\d3dx9_25.dll" copy /y "%%F" "%GAME%\d3dx9_25.dll" >nul
    set "FOUND=1"
  )
  if not defined FOUND echo warning: could not locate d3dx9_25.dll anywhere under game dir
)

echo [7/7] deleting old log
if exist "%GAME%\d3d9mod.log" del "%GAME%\d3d9mod.log"
if exist "%ROOT%\tests\d3d9mod.log" del "%ROOT%\tests\d3d9mod.log"
if exist "%ROOT%\d3d9mod.log" del "%ROOT%\d3d9mod.log"

popd
echo DONE
exit /b 0