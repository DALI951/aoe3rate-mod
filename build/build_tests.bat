@echo off
setlocal
set "W64=C:\Users\dali\AppData\Local\Temp\opencode\w64devkit-x86\w64devkit"
set "PATH=%W64%\bin;%W64%\libexec\gcc\i686-w64-mingw32\16.2.0;%PATH%"
pushd "%~dp0.."

echo === Building test_export.exe ===
i686-w64-mingw32-gcc.exe tests\test_export.c -o tests\test_export.exe -luser32 -lwinmm 2>&1
echo rc=%ERRORLEVEL%

echo === Building test_rate_engine.exe ===
i686-w64-mingw32-gcc.exe tests\test_rate_engine.c -o tests\test_rate_engine.exe -luser32 -lwinmm 2>&1
echo rc=%ERRORLEVEL%

echo === Building test_d3d9_actual.exe ===
i686-w64-mingw32-gcc.exe tests\test_d3d9_actual.c -o tests\test_d3d9_actual.exe -luser32 -lwinmm 2>&1
echo rc=%ERRORLEVEL%

popd
