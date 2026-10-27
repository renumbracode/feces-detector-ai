@echo off
REM Internal helper for scripts\start_verifier.cmd -- do not run directly.
REM
REM This file exists so the detached server can be launched with `start`, which
REM does NOT inherit the caller's handles (CreateProcess with
REM bInheritHandles=FALSE). Doing the redirection here, inside the child, is
REM what makes stdout/stderr land in the log files instead of in whatever pipe
REM the parent shell was capturing.
REM
REM All configuration arrives through environment variables set by the parent:
REM   VERIFY_APPDIR VERIFY_PYEXE VERIFY_PYARGS VERIFY_HOST VERIFY_PORT
REM   VERIFY_LOG VERIFY_ERRLOG
cd /d "%VERIFY_APPDIR%"
"%VERIFY_PYEXE%" %VERIFY_PYARGS% -m uvicorn app:app --host %VERIFY_HOST% --port %VERIFY_PORT% 1>>"%VERIFY_LOG%" 2>>"%VERIFY_ERRLOG%"
