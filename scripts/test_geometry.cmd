@echo off
REM Host-side test for the geometry solver.
REM
REM The solver is the part of the actuator board most likely to be wrong, and
REM it is a pure function precisely so it can be checked on a laptop -- no
REM board, no servo, no pig pen.
REM
REM Usage:  scripts\test_geometry.cmd
setlocal
set "HERE=%~dp0"
set "CC=gcc"
where %CC% >nul 2>&1
if errorlevel 1 (
    echo [test] gcc not found on PATH.
    exit /b 1
)
%CC% -std=c17 -Wall -Wextra -O1 ^
    -I"%HERE%..\firmware\esp32_main\components\geometry\include" ^
    "%HERE%..\firmware\esp32_main\components\geometry\geometry.c" ^
    "%HERE%..\firmware\esp32_main\components\geometry\test\test_geometry.c" ^
    -o "%TEMP%\test_geometry.exe" -lm
if errorlevel 1 (
    echo [test] BUILD FAILED
    exit /b 1
)
"%TEMP%\test_geometry.exe"
if errorlevel 1 (
    echo [test] TESTS FAILED
    exit /b 1
)
echo [test] all geometry tests passed
endlocal
