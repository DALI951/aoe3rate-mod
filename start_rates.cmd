@echo off
REM AOE3 Rates live viewer launcher (zero-dependency fallback).
REM Identical copy works from the repo root AND the game folder: the app always
REM lives at the absolute repo path below, so both copies resolve the same files.

set "PYW=C:\Users\dali\AppData\Local\Programs\Python\Python312\pythonw.exe"
set "APP=C:\Users\dali\aoe3rate-mod\app\app.py"
set "WORK=C:\Users\dali\aoe3rate-mod\app"

if not exist "%PYW%" (
    echo [AOE3 Rates] pythonw.exe not found at "%PYW%" >&2
    pause
    exit /b 1
)
if not exist "%APP%" (
    echo [AOE3 Rates] app.py not found at "%APP%" >&2
    pause
    exit /b 1
)

pushd "%WORK%"
start "" "%PYW%" "%APP%"
popd