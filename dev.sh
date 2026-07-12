#!/usr/bin/env bash
# dev.sh — launch backend + frontend for fractal_studio
# Usage: ./dev.sh [--backend-port PORT] [--frontend-port PORT]
#
# Backend port default: 18080
# Frontend port default: 5174
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BACKEND_BUILD_DIR="$SCRIPT_DIR/runtime/build"
BACKEND_BIN="$BACKEND_BUILD_DIR/fractal_studio_backend"
FRONTEND_DIR="$SCRIPT_DIR/frontend"
BACKEND_PORT="${BACKEND_PORT:-18080}"
FRONTEND_PORT="${FRONTEND_PORT:-5174}"
BACKEND_PID=""
FRONTEND_PID=""
TAIL_PID=""

stop_process() {
    local pid="$1"
    if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
}

cleanup() {
    stop_process "$TAIL_PID"
    stop_process "$FRONTEND_PID"
    stop_process "$BACKEND_PID"
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# Parse args
while [[ $# -gt 0 ]]; do
    case "$1" in
        --backend-port)  BACKEND_PORT="$2"; shift 2 ;;
        --frontend-port) FRONTEND_PORT="$2"; shift 2 ;;
        *) echo "Unknown arg: $1"; exit 1 ;;
    esac
done

# Keep the dev backend in sync with the Makefile target. Starting an old
# backend binary is especially confusing because the frontend can look current
# while /api/map/render still runs stale transition logic.
echo "[dev.sh] Building backend ..."
cmake -S "$SCRIPT_DIR/backend" -B "$BACKEND_BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BACKEND_BUILD_DIR" -j"$(nproc)"

# Free ports if already in use. Use fuser -k for reliable kill, then poll
# until the port is actually released (avoids the race where the new process
# binds before the old one has fully closed its socket).
free_port() {
    local port="$1"
    if fuser -k "${port}/tcp" 2>/dev/null; then
        echo "[dev.sh] Freeing port $port..."
        local waited=0
        while fuser "${port}/tcp" &>/dev/null && [[ $waited -lt 30 ]]; do
            sleep 0.1
            (( waited++ ))
        done
    fi
}
free_port "$BACKEND_PORT"
free_port "$FRONTEND_PORT"

LOG_DIR="$SCRIPT_DIR/runtime/logs"
mkdir -p "$LOG_DIR"

# Backend: run from the workspace root; the binary locates fractal_studio by
# backend/frontend markers, not by any legacy source directory.
echo "[dev.sh] Starting backend on :$BACKEND_PORT ..."
(cd "$REPO_ROOT" && exec "$BACKEND_BIN" "$BACKEND_PORT") \
    > "$LOG_DIR/backend.log" 2>&1 &
BACKEND_PID=$!
echo "         PID=$BACKEND_PID  log=$LOG_DIR/backend.log"

# Wait briefly so the backend socket is open before vite starts
sleep 0.8

echo "[dev.sh] Starting frontend on :$FRONTEND_PORT ..."
(cd "$FRONTEND_DIR" && \
    exec npx vite --port "$FRONTEND_PORT" --host --strictPort) \
    > "$LOG_DIR/frontend.log" 2>&1 &
FRONTEND_PID=$!
echo "         PID=$FRONTEND_PID  log=$LOG_DIR/frontend.log"

echo ""
echo "  Backend  → http://localhost:$BACKEND_PORT"
echo "  Frontend → http://localhost:$FRONTEND_PORT"
echo "  Press Ctrl-C to stop both."
echo ""

# Stream both logs to the terminal
tail -n 0 -f "$LOG_DIR/backend.log" "$LOG_DIR/frontend.log" &
TAIL_PID=$!

# Exit when the backend dies (frontend is secondary)
wait "$BACKEND_PID" || true
echo "[dev.sh] Backend exited."
