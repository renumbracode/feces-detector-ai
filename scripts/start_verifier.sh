#!/usr/bin/env bash
# Start the YOLOv8 verifier (server/yolo_verify) detached, with stdout/stderr
# redirected to files under logs/.
#
# Why this exists: the verifier MUST NOT inherit stdout/stderr from whatever
# shell launched it. If it does, the parent is usually a tool/session capture
# pipe that stops being drained once that call ends. The pipe fills, python
# blocks inside write(), and uvicorn -- which binds the socket only AFTER the
# app's startup event -- never gets to create_server(). The result is a
# process that looks alive (1.5 GB RSS, 343 threads, torch fully loaded) but
# answers nothing on :8000, and the board reports verifyOkAgeMs = UINT64_MAX
# forever. That exact failure cost a session on 2026-10-27.
#
# Redirecting to a file removes the pipe entirely, so the server cannot wedge.
#
# Usage:  scripts/start_verifier.sh
#         scripts/start_verifier.sh stop
#
# Env overrides:
#   YOLO_VERIFY_PY   python interpreter to use (default: python)
#   VERIFY_HOST      bind address (default 0.0.0.0)
#   VERIFY_PORT      port (default 8000)
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APPDIR="$REPO/server/yolo_verify"
LOGDIR="$REPO/logs"
LOG="$LOGDIR/yolo_verify.log"
ERRLOG="$LOGDIR/yolo_verify.err.log"
PIDFILE="$LOGDIR/yolo_verify.pid"

HOST="${VERIFY_HOST:-0.0.0.0}"
PORT="${VERIFY_PORT:-8000}"
PY="${YOLO_VERIFY_PY:-python}"

mkdir -p "$LOGDIR"

running_pid() {
    netstat -ano -p TCP 2>/dev/null \
        | grep -E "[:.]${PORT} +.*LISTENING" \
        | awk '{print $NF}' \
        | head -n1
}

if [ "${1:-start}" = "stop" ]; then
    pid="$(running_pid || true)"
    if [ -z "${pid:-}" ]; then
        echo "[verify] Nothing listening on port $PORT."
        exit 0
    fi
    echo "[verify] Stopping PID $pid ..."
    taskkill //PID "$pid" //F >/dev/null 2>&1 || kill -9 "$pid" 2>/dev/null || true
    rm -f "$PIDFILE"
    echo "[verify] Stopped."
    exit 0
fi

pid="$(running_pid || true)"
if [ -n "${pid:-}" ]; then
    echo "[verify] Already running, PID $pid on port $PORT."
    echo "[verify] Nothing to do. Use 'scripts/start_verifier.sh stop' first."
    exit 0
fi

echo "[verify] Starting verifier on $HOST:$PORT"
echo "[verify] stdout -> $LOG"
echo "[verify] stderr -> $ERRLOG"

cd "$APPDIR"

# setsid puts the server in a NEW SESSION, which is what actually detaches
# it: nohup alone only ignores SIGHUP, so a Ctrl+C delivered to the
# foreground process group still reaches the server and kills it with a
# `^C`. This is the same failure the Windows launcher had, and it is why the
# launcher there uses CreateNoWindow (no console at all).
#
# setsid is not present everywhere (notably not on macOS), so fall back to
# the nohup form when it is missing.
if command -v setsid >/dev/null 2>&1; then
    setsid "$PY" -m uvicorn app:app --host "$HOST" --port "$PORT" \
        >"$LOG" 2>"$ERRLOG" </dev/null &
else
    nohup "$PY" -m uvicorn app:app --host "$HOST" --port "$PORT" \
        >"$LOG" 2>"$ERRLOG" </dev/null &
fi
LAUNCH_PID=$!
disown 2>/dev/null || true

# A bound socket is not enough: /health must report model_loaded:true, i.e. the
# YOLOv8 weights are in memory and the first /verify will not have to pay the
# model-load cost (the board's own HTTP timeout cannot absorb it).
for _ in $(seq 1 120); do
    health="$(curl -s -m 2 "http://127.0.0.1:$PORT/health" 2>/dev/null || true)"
    case "$health" in
        *'"model_loaded":true'*)
            # Record the PID that actually owns the listening socket, not the
            # launcher PID: MSYS nohup may exec the interpreter through a
            # wrapper, so $! is not always the server.
            PID="$(running_pid || true)"
            [ -n "${PID:-}" ] && echo "$PID" > "$PIDFILE"
            echo "$health"
            echo "[verify] Running, PID ${PID:-unknown} (launcher $LAUNCH_PID)."
            exit 0
            ;;
    esac
    if ! kill -0 "$LAUNCH_PID" 2>/dev/null; then
        echo "[verify] Process exited during startup. Tail of $ERRLOG:"
        tail -n 30 "$ERRLOG" || true
        exit 1
    fi
    sleep 1
done

echo "[verify] did not become healthy within 120s. Tail of $ERRLOG:"
tail -n 30 "$ERRLOG" || true
exit 1
