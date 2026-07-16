#!/usr/bin/env bash
# dev.sh — launch backend + frontend for fractal_studio
# Usage: ./dev.sh [--backend-port PORT] [--frontend-port PORT]
#
# Backend port default: 18080
# Frontend port default: 5174
set -euo pipefail

if (( BASH_VERSINFO[0] < 4 || (BASH_VERSINFO[0] == 4 && BASH_VERSINFO[1] < 3) )); then
    echo "[dev.sh] Bash 4.3 or newer is required." >&2
    exit 2
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BACKEND_BUILD_DIR="$SCRIPT_DIR/runtime/build"
BACKEND_BIN="$BACKEND_BUILD_DIR/fractal_studio_backend"
FRONTEND_DIR="$SCRIPT_DIR/frontend"
VITE_BIN="$FRONTEND_DIR/node_modules/.bin/vite"
BACKEND_PORT="${BACKEND_PORT:-18080}"
FRONTEND_PORT="${FRONTEND_PORT:-5174}"
BACKEND_PID=""
BACKEND_GROUP_PID=""
FRONTEND_PID=""
FRONTEND_GROUP_PID=""
TAIL_PID=""
LAUNCHED_PID=""
LAUNCHED_GROUP_PID=""
LAUNCHED_PID_FILE=""

usage() {
    echo "Usage: $0 [--backend-port PORT] [--frontend-port PORT]" >&2
}

fail() {
    echo "[dev.sh] $*" >&2
    exit 1
}

validate_port() {
    local name="$1"
    local port="$2"
    if [[ ! "$port" =~ ^[0-9]{1,5}$ ]]; then
        fail "$name must be an integer in 1..65535 (got: $port)"
    fi
    local decimal_port=$((10#$port))
    if (( decimal_port < 1 || decimal_port > 65535 )); then
        fail "$name must be an integer in 1..65535 (got: $port)"
    fi
}

process_is_running() {
    local pid="$1"
    local state
    state="$(ps -o stat= -p "$pid" 2>/dev/null)" || return 1
    [[ -n "$state" && "${state:0:1}" != "Z" ]]
}

stop_process_group() {
    local supervisor_pid="$1"
    local group_pid="$2"
    [[ -n "$supervisor_pid" || -n "$group_pid" ]] || return 0

    # The service runs in a forced-fork setsid child. Kill that recorded session
    # group, then reap its small `setsid --wait` supervisor separately.
    if [[ -n "$group_pid" ]]; then
        kill -TERM -- "-$group_pid" 2>/dev/null || true
    elif [[ -n "$supervisor_pid" ]]; then
        kill -TERM "$supervisor_pid" 2>/dev/null || true
    fi
    local waited=0
    while [[ -n "$supervisor_pid" ]] && process_is_running "$supervisor_pid" && (( waited < 30 )); do
        sleep 0.1
        waited=$((waited + 1))
    done
    if [[ -n "$group_pid" ]]; then
        kill -KILL -- "-$group_pid" 2>/dev/null || true
    fi
    if [[ -n "$supervisor_pid" ]]; then
        kill -KILL "$supervisor_pid" 2>/dev/null || true
        wait "$supervisor_pid" 2>/dev/null || true
    fi
}

stop_process() {
    local pid="$1"
    if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
        kill -TERM "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
}

stop_pending_session() {
    local attempt
    if [[ -n "$LAUNCHED_PID" && -z "$LAUNCHED_GROUP_PID" && -n "$LAUNCHED_PID_FILE" ]]; then
        for attempt in {1..20}; do
            if [[ -s "$LAUNCHED_PID_FILE" ]]; then
                IFS= read -r LAUNCHED_GROUP_PID < "$LAUNCHED_PID_FILE"
                if [[ ! "$LAUNCHED_GROUP_PID" =~ ^[0-9]+$ ]]; then
                    LAUNCHED_GROUP_PID=""
                fi
                break
            fi
            process_is_running "$LAUNCHED_PID" || break
            sleep 0.01
        done
    fi
    stop_process_group "$LAUNCHED_PID" "$LAUNCHED_GROUP_PID"
    if [[ -n "$LAUNCHED_PID_FILE" ]]; then
        rm -f "$LAUNCHED_PID_FILE"
    fi
    LAUNCHED_PID=""
    LAUNCHED_GROUP_PID=""
    LAUNCHED_PID_FILE=""
}

cleanup() {
    stop_process "$TAIL_PID"
    stop_pending_session
    stop_process_group "$FRONTEND_PID" "$FRONTEND_GROUP_PID"
    stop_process_group "$BACKEND_PID" "$BACKEND_GROUP_PID"
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# Parse args
while [[ $# -gt 0 ]]; do
    case "$1" in
        --backend-port)
            [[ $# -ge 2 ]] || { usage; fail "--backend-port requires a value"; }
            BACKEND_PORT="$2"
            shift 2
            ;;
        --frontend-port)
            [[ $# -ge 2 ]] || { usage; fail "--frontend-port requires a value"; }
            FRONTEND_PORT="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage
            fail "unknown argument: $1"
            ;;
    esac
done

validate_port "backend port" "$BACKEND_PORT"
validate_port "frontend port" "$FRONTEND_PORT"
BACKEND_PORT="$((10#$BACKEND_PORT))"
FRONTEND_PORT="$((10#$FRONTEND_PORT))"
if (( BACKEND_PORT == FRONTEND_PORT )); then
    fail "backend and frontend ports must be different"
fi

if [[ ! -x "$VITE_BIN" ]]; then
    fail "Vite is not installed; run 'cd $FRONTEND_DIR && npm install' first"
fi
if ! command -v setsid >/dev/null 2>&1 || ! setsid --help 2>&1 | grep -q -- '--fork'; then
    fail "util-linux setsid with --fork is required to manage service process groups"
fi

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
    if ! command -v fuser >/dev/null 2>&1; then
        fail "fuser is required to check and release development ports"
    fi
    if ! fuser "${port}/tcp" >/dev/null 2>&1; then
        return 0
    fi

    echo "[dev.sh] Freeing port $port..."
    if ! fuser -k -TERM "${port}/tcp" >/dev/null 2>&1; then
        fail "could not stop the process using port $port"
    fi

    local waited=0
    while fuser "${port}/tcp" >/dev/null 2>&1; do
        if (( waited >= 30 )); then
            fail "port $port is still in use after 3 seconds"
        fi
        sleep 0.1
        waited=$((waited + 1))
    done
}
free_port "$BACKEND_PORT"
free_port "$FRONTEND_PORT"

LOG_DIR="$SCRIPT_DIR/runtime/logs"
mkdir -p "$LOG_DIR"

launch_session() {
    local working_dir="$1"
    local log_file="$2"
    local pid_file="$3"
    shift 3

    LAUNCHED_PID_FILE="$pid_file"
    rm -f "$pid_file"
    (
        cd "$working_dir"
        exec 3>"$log_file"
        exec 4>&3
        exec setsid --fork --wait bash -c '
            pid_file=$1
            shift
            if ! printf "%s\n" "$$" > "$pid_file"; then
                exit 125
            fi
            exec "$@" >&3 2>&4
        ' fsd-session "$pid_file" "$@"
    ) > /dev/null 2>&1 &
    LAUNCHED_PID=$!
    LAUNCHED_GROUP_PID=""

    local attempt
    for attempt in {1..100}; do
        if [[ -s "$pid_file" ]]; then
            IFS= read -r LAUNCHED_GROUP_PID < "$pid_file"
            rm -f "$pid_file"
            if [[ "$LAUNCHED_GROUP_PID" =~ ^[0-9]+$ ]] && kill -0 "$LAUNCHED_GROUP_PID" 2>/dev/null; then
                LAUNCHED_PID_FILE=""
                return 0
            fi
            break
        fi
        if ! process_is_running "$LAUNCHED_PID"; then
            break
        fi
        sleep 0.01
    done

    rm -f "$pid_file"
    stop_process_group "$LAUNCHED_PID" "$LAUNCHED_GROUP_PID"
    LAUNCHED_PID=""
    LAUNCHED_GROUP_PID=""
    LAUNCHED_PID_FILE=""
    return 1
}

# Backend: run from the studio root so root discovery does not depend on the
# checkout directory being named "fractal_studio".
echo "[dev.sh] Starting backend on :$BACKEND_PORT ..."
if ! launch_session "$SCRIPT_DIR" "$LOG_DIR/backend.log" \
    "$LOG_DIR/.backend-session.$$" "$BACKEND_BIN" "$BACKEND_PORT"; then
    fail "could not start the backend session"
fi
BACKEND_PID="$LAUNCHED_PID"
BACKEND_GROUP_PID="$LAUNCHED_GROUP_PID"
LAUNCHED_PID=""
LAUNCHED_GROUP_PID=""
echo "         PID=$BACKEND_GROUP_PID  supervisor=$BACKEND_PID  log=$LOG_DIR/backend.log"

backend_is_ready() {
    if command -v curl >/dev/null 2>&1; then
        curl -fsS --max-time 1 \
            "http://127.0.0.1:${BACKEND_PORT}/api/system/check" >/dev/null 2>&1
    else
        (exec 3<>"/dev/tcp/127.0.0.1/${BACKEND_PORT}") >/dev/null 2>&1
    fi
}

backend_logged_ready() {
    grep -Fq "HTTP server listening on 0.0.0.0:${BACKEND_PORT}" "$LOG_DIR/backend.log" 2>/dev/null
}

wait_for_backend() {
    local attempt
    local backend_status
    for attempt in {1..60}; do
        if ! kill -0 "$BACKEND_PID" 2>/dev/null; then
            backend_status=0
            if wait "$BACKEND_PID"; then
                backend_status=0
            else
                backend_status=$?
            fi
            echo "[dev.sh] Backend exited during startup (status $backend_status)." >&2
            tail -n 20 "$LOG_DIR/backend.log" >&2 || true
            return 1
        fi
        if backend_logged_ready && backend_is_ready && kill -0 "$BACKEND_PID" 2>/dev/null; then
            return 0
        fi
        sleep 0.1
    done

    echo "[dev.sh] Backend did not become ready on port $BACKEND_PORT." >&2
    tail -n 20 "$LOG_DIR/backend.log" >&2 || true
    return 1
}

wait_for_backend || exit 1

# Pass the selected port separately so api.ts can combine it with the browser's
# location.hostname. This keeps custom ports working for both localhost and LAN
# devices. An explicit VITE_BACKEND_URL remains the highest-priority override.
FRONTEND_BACKEND_URL="${VITE_BACKEND_URL:-}"

echo "[dev.sh] Starting frontend on :$FRONTEND_PORT ..."
frontend_command=(env "VITE_BACKEND_PORT=$BACKEND_PORT")
if [[ -n "$FRONTEND_BACKEND_URL" ]]; then
    frontend_command+=("VITE_BACKEND_URL=$FRONTEND_BACKEND_URL")
fi
frontend_command+=("$VITE_BIN" --port "$FRONTEND_PORT" --host --strictPort)
if ! launch_session "$FRONTEND_DIR" "$LOG_DIR/frontend.log" \
    "$LOG_DIR/.frontend-session.$$" "${frontend_command[@]}"; then
    fail "could not start the frontend session"
fi
FRONTEND_PID="$LAUNCHED_PID"
FRONTEND_GROUP_PID="$LAUNCHED_GROUP_PID"
LAUNCHED_PID=""
LAUNCHED_GROUP_PID=""
echo "         PID=$FRONTEND_GROUP_PID  supervisor=$FRONTEND_PID  log=$LOG_DIR/frontend.log"

echo ""
echo "  Backend  → http://localhost:$BACKEND_PORT"
echo "  Frontend → http://localhost:$FRONTEND_PORT"
echo "  Press Ctrl-C to stop both."
echo ""

# Stream both logs to the terminal
tail -n 0 -f "$LOG_DIR/backend.log" "$LOG_DIR/frontend.log" &
TAIL_PID=$!

# Either service exiting makes the development stack unusable. Poll the two
# service leaders explicitly: Bash 4.3 has `wait -n`, but it did not yet accept
# a PID list (and an unrestricted wait could select the log-tail process).
while process_is_running "$BACKEND_PID" && process_is_running "$FRONTEND_PID"; do
    sleep 0.1
done

child_status=0
if ! process_is_running "$BACKEND_PID"; then
    if wait "$BACKEND_PID"; then
        child_status=0
    else
        child_status=$?
    fi
else
    if wait "$FRONTEND_PID"; then
        child_status=0
    else
        child_status=$?
    fi
fi

exited_services=()
if ! kill -0 "$BACKEND_PID" 2>/dev/null; then
    exited_services+=("backend")
fi
if ! kill -0 "$FRONTEND_PID" 2>/dev/null; then
    exited_services+=("frontend")
fi
if (( ${#exited_services[@]} == 0 )); then
    exited_services+=("service")
fi

echo "[dev.sh] ${exited_services[*]} exited (status $child_status); stopping the development stack." >&2
if (( child_status == 0 )); then
    exit 1
fi
exit "$child_status"
