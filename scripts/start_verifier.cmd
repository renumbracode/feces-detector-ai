@echo off
REM Start the YOLOv8 verifier (server/yolo_verify) as a detached background
REM process whose stdout/stderr go to files under logs\.
REM
REM Why this exists: the verifier MUST NOT inherit stdout/stderr from whatever
REM shell launched it. If it does, the parent is usually a tool/session capture
REM pipe that stops being drained once that call ends. The pipe fills, python
REM blocks inside write(), and uvicorn -- which binds the socket only AFTER
REM the app's startup event -- never gets to create_server(). The result is a
REM process that looks alive (1.5 GB RSS, 343 threads, torch fully loaded) but
REM answers nothing on :8000, and the board reports verifyOkAgeMs =
REM UINT64_MAX forever. That exact failure cost a session on 2026-10-27.
REM
REM Redirecting to a FILE removes the pipe entirely, so the server cannot wedge.
REM
REM Why this no longer uses `start /min cmd /c`: that creates a CONSOLE, and a
REM console can receive a Ctrl+C. Closing that window -- or any Ctrl+C reaching
REM the console group -- killed the server with a `^C` in the error log. It
REM happened TWICE in one session (2026-10-27). The launch below uses
REM CreateNoWindow, so the server has NO console at all and nothing can
REM Ctrl+C it: the only way to stop it is `start_verifier.cmd stop`.
REM
REM Usage:  scripts\start_verifier.cmd
REM         scripts\start_verifier.cmd stop     (kill a running verifier)
REM
REM Env overrides:
REM   YOLO_VERIFY_PY   full path to the Python interpreter to use
REM   VERIFY_HOST      bind address (default 0.0.0.0)
REM   VERIFY_PORT      port (default 8000)
setlocal enabledelayedexpansion

set "REPO=%~dp0.."
set "APPDIR=%REPO%\server\yolo_verify"
set "LOGDIR=%REPO%\logs"
set "LOG=%LOGDIR%\yolo_verify.log"
set "ERRLOG=%LOGDIR%\yolo_verify.err.log"
set "PIDFILE=%LOGDIR%\yolo_verify.pid"
set "HOST=%VERIFY_HOST%"
if not defined HOST set "HOST=0.0.0.0"
set "PORT=%VERIFY_PORT%"
if not defined PORT set "PORT=8000"

if /i "%~1"=="stop" goto :stop

if not exist "%LOGDIR%" mkdir "%LOGDIR%"

REM ---- pick an interpreter -------------------------------------------------
set "PYEXE="
set "PYARGS="
if defined YOLO_VERIFY_PY (
    set "PYEXE=%YOLO_VERIFY_PY%"
) else (
    for /f "delims=" %%i in ('where python.exe 2^>nul') do if not defined PYEXE set "PYEXE=%%i"
    if not defined PYEXE (
        for /f "delims=" %%i in ('where py.exe 2^>nul') do if not defined PYEXE set "PYEXE=%%i"
        if defined PYEXE set "PYARGS=-3"
    )
)
if not defined PYEXE (
    echo [verify] No Python interpreter found. Set YOLO_VERIFY_PY to one and retry.
    exit /b 1
)

REM ---- refuse to double-start ---------------------------------------------
call :running_pid
if defined RUNPID (
    echo [verify] Already running, PID !RUNPID! on port %PORT%.
    echo [verify] Nothing to do. Use "scripts\start_verifier.cmd stop" first.
    exit /b 0
)

echo [verify] Starting verifier on %HOST%:%PORT% as !PYEXE! !PYARGS!
echo [verify] stdout -^> %LOG%
echo [verify] stderr -^> %ERRLOG%

REM ---- launch it with NO console ------------------------------------------
REM The inner command redirects to FILES, never to a pipe -- see the header.
REM CreateNoWindow means the child has no console, so no Ctrl+C can reach it.
REM The values travel through the environment rather than the command line so
REM that paths with spaces need no quoting gymnastics.
set "VERIFY_APPDIR=%APPDIR%"
set "VERIFY_PYEXE=!PYEXE!"
set "VERIFY_PYARGS=!PYARGS!"
set "VERIFY_HOST=%HOST%"
set "VERIFY_PORT=%PORT%"
set "VERIFY_LOG=%LOG%"
set "VERIFY_ERRLOG=%ERRLOG%"

powershell -NoProfile -Command ^
  "$inner = 'cd /d \"' + $env:VERIFY_APPDIR + '\" && \"' + $env:VERIFY_PYEXE + '\" ' + $env:VERIFY_PYARGS + ' -m uvicorn app:app --host ' + $env:VERIFY_HOST + ' --port ' + $env:VERIFY_PORT + ' 1>>\"' + $env:VERIFY_LOG + '\" 2>>\"' + $env:VERIFY_ERRLOG + '\"';" ^
  "$psi = New-Object System.Diagnostics.ProcessStartInfo;" ^
  "$psi.FileName = $env:ComSpec;" ^
  "$psi.Arguments = '/c ' + $inner;" ^
  "$psi.UseShellExecute = $false;" ^
  "$psi.CreateNoWindow = $true;" ^
  "$psi.WorkingDirectory = $env:VERIFY_APPDIR;" ^
  "[void][System.Diagnostics.Process]::Start($psi)"

REM ---- wait until it is actually warm --------------------------------------
REM A bound socket is not enough: /health must report model_loaded:true, i.e.
REM the YOLOv8 weights are in memory and the first /verify will not have to
REM pay the model-load cost (the board's own HTTP timeout cannot absorb it).
powershell -NoProfile -Command ^
  "$deadline=(Get-Date).AddSeconds(120);" ^
  "do { try { $h=Invoke-RestMethod -Uri 'http://127.0.0.1:%PORT%/health' -TimeoutSec 2;" ^
  "if($h.ok -and $h.model_loaded){ $h | ConvertTo-Json -Compress; exit 0 } } catch {};" ^
  "Start-Sleep -Seconds 1 } while((Get-Date) -lt $deadline);" ^
  "Write-Output '[verify] did not become healthy within 120s'; exit 1"
if errorlevel 1 (
    echo [verify] Startup failed. Tail of %ERRLOG%:
    powershell -NoProfile -Command "Get-Content -Tail 30 '%ERRLOG%'"
    exit /b 1
)

REM ---- record the PID so `stop` works --------------------------------------
call :running_pid
if defined RUNPID (
    echo !RUNPID!> "%PIDFILE%"
    echo [verify] Running, PID !RUNPID!.
) else (
    REM No parens in this echo: an unescaped ")" inside a parenthesised block
    REM closes the block early, and cmd then tries to run the remaining text
    REM as a command -- which is where ". was unexpected at this time." came
    REM from.
    echo [verify] Started, PID unknown - use the stop argument to kill it.
)
exit /b 0

:stop
call :running_pid
if not defined RUNPID (
    echo [verify] Nothing listening on port %PORT%.
    exit /b 0
)
echo [verify] Stopping PID !RUNPID! ...
powershell -NoProfile -Command "Stop-Process -Id !RUNPID! -Force" >nul 2>&1
if exist "%PIDFILE%" del "%PIDFILE%"
echo [verify] Stopped.
exit /b 0

REM ---- helper: find the PID listening on the port -------------------------
:running_pid
set "RUNPID="
for /f "tokens=5" %%p in ('netstat -ano -p TCP ^| findstr /r /c:":%PORT% .*LISTENING"') do (
    set "RUNPID=%%p"
)
exit /b 0
