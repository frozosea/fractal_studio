#!/usr/bin/env bash
set -euo pipefail

if (( BASH_VERSINFO[0] < 4 || (BASH_VERSINFO[0] == 4 && BASH_VERSINFO[1] < 3) )); then
  echo "Bash 4.3 or newer is required." >&2
  exit 2
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BACKEND_BUILD_DIR="${ROOT}/runtime/build"
BACKEND_BIN="${BACKEND_BUILD_DIR}/fractal_studio_backend"
FRONTEND_DIR="${ROOT}/frontend"
VITE_BIN="${FRONTEND_DIR}/node_modules/.bin/vite"
BACKEND_PORT=18080
BACKEND_PID=""
BACKEND_GROUP_PID=""
FRONTEND_PID=""
FRONTEND_GROUP_PID=""
LAUNCHED_PID=""
LAUNCHED_GROUP_PID=""
LAUNCHED_PID_FILE=""

process_is_running() {
  local pid="$1"
  local state
  state="$(ps -o stat= -p "${pid}" 2>/dev/null)" || return 1
  [[ -n "${state}" && "${state:0:1}" != "Z" ]]
}

stop_process_group() {
  local supervisor_pid="$1"
  local group_pid="$2"
  [[ -n "${supervisor_pid}" || -n "${group_pid}" ]] || return 0

  if [[ -n "${group_pid}" ]]; then
    kill -TERM -- "-${group_pid}" 2>/dev/null || true
  elif [[ -n "${supervisor_pid}" ]]; then
    kill -TERM "${supervisor_pid}" 2>/dev/null || true
  fi
  local waited=0
  while [[ -n "${supervisor_pid}" ]] && process_is_running "${supervisor_pid}" && (( waited < 30 )); do
    sleep 0.1
    waited=$((waited + 1))
  done
  if [[ -n "${group_pid}" ]]; then
    kill -KILL -- "-${group_pid}" 2>/dev/null || true
  fi
  if [[ -n "${supervisor_pid}" ]]; then
    kill -KILL "${supervisor_pid}" 2>/dev/null || true
    wait "${supervisor_pid}" 2>/dev/null || true
  fi
}

stop_pending_session() {
  local attempt
  if [[ -n "${LAUNCHED_PID}" && -z "${LAUNCHED_GROUP_PID}" && -n "${LAUNCHED_PID_FILE}" ]]; then
    for attempt in {1..20}; do
      if [[ -s "${LAUNCHED_PID_FILE}" ]]; then
        IFS= read -r LAUNCHED_GROUP_PID < "${LAUNCHED_PID_FILE}"
        if [[ ! "${LAUNCHED_GROUP_PID}" =~ ^[0-9]+$ ]]; then
          LAUNCHED_GROUP_PID=""
        fi
        break
      fi
      process_is_running "${LAUNCHED_PID}" || break
      sleep 0.01
    done
  fi
  stop_process_group "${LAUNCHED_PID}" "${LAUNCHED_GROUP_PID}"
  if [[ -n "${LAUNCHED_PID_FILE}" ]]; then
    rm -f "${LAUNCHED_PID_FILE}"
  fi
  LAUNCHED_PID=""
  LAUNCHED_GROUP_PID=""
  LAUNCHED_PID_FILE=""
}

cleanup() {
  stop_pending_session
  stop_process_group "${FRONTEND_PID}" "${FRONTEND_GROUP_PID}"
  stop_process_group "${BACKEND_PID}" "${BACKEND_GROUP_PID}"
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

port_is_listening() {
  local port="$1"
  if command -v ss >/dev/null 2>&1; then
    ss -H -ltn "sport = :${port}" 2>/dev/null | grep -q .
    return
  fi
  if command -v fuser >/dev/null 2>&1; then
    fuser "${port}/tcp" >/dev/null 2>&1
    return
  fi
  (exec 3<>"/dev/tcp/127.0.0.1/${port}") >/dev/null 2>&1
}

backend_is_ready() {
  if command -v curl >/dev/null 2>&1; then
    curl -fsS --max-time 1 \
      "http://127.0.0.1:${BACKEND_PORT}/api/system/check" >/dev/null 2>&1
  else
    port_is_listening "${BACKEND_PORT}"
  fi
}

wait_for_backend() {
  local attempt
  local backend_status
  for attempt in {1..60}; do
    if ! kill -0 "${BACKEND_PID}" 2>/dev/null; then
      backend_status=0
      if wait "${BACKEND_PID}"; then
        backend_status=0
      else
        backend_status=$?
      fi
      echo "Backend exited before becoming ready (status ${backend_status})" >&2
      return 1
    fi
    if backend_is_ready && kill -0 "${BACKEND_PID}" 2>/dev/null; then
      return 0
    fi
    sleep 0.5
  done
  echo "Backend did not become ready on port ${BACKEND_PORT}" >&2
  return 1
}

if ! command -v cmake >/dev/null 2>&1; then
  echo "cmake not found" >&2
  exit 1
fi

if ! command -v npm >/dev/null 2>&1; then
  echo "npm not found" >&2
  exit 1
fi

if ! command -v setsid >/dev/null 2>&1 || ! setsid --help 2>&1 | grep -q -- '--fork'; then
  echo "util-linux setsid with --fork not found" >&2
  exit 1
fi

echo "[1/4] Building backend"
cmake -S "${ROOT}/backend" -B "${BACKEND_BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${BACKEND_BUILD_DIR}" -j

if [[ ! -x "${BACKEND_BIN}" ]]; then
  echo "Backend binary not found: ${BACKEND_BIN}" >&2
  exit 1
fi

if [[ ! -x "${VITE_BIN}" ]]; then
  echo "[2/4] Installing frontend dependencies"
  (cd "${FRONTEND_DIR}" && npm ci)
fi

if port_is_listening "${BACKEND_PORT}"; then
  echo "Port ${BACKEND_PORT} is already in use; refusing to attach to an existing backend." >&2
  echo "Stop the existing process, or use ./dev.sh --backend-port PORT for a separate stack." >&2
  exit 1
fi

launch_session() {
  local working_dir="$1"
  local pid_file="$2"
  shift 2

  LAUNCHED_PID_FILE="${pid_file}"
  rm -f "${pid_file}"
  (
    cd "${working_dir}"
    exec 3>&1
    exec 4>&2
    exec setsid --fork --wait bash -c '
      pid_file=$1
      shift
      if ! printf "%s\n" "$$" > "$pid_file"; then
        exit 125
      fi
      exec "$@" >&3 2>&4
    ' fsd-session "${pid_file}" "$@" 2>/dev/null
  ) &
  LAUNCHED_PID=$!
  LAUNCHED_GROUP_PID=""

  local attempt
  for attempt in {1..100}; do
    if [[ -s "${pid_file}" ]]; then
      IFS= read -r LAUNCHED_GROUP_PID < "${pid_file}"
      rm -f "${pid_file}"
      if [[ "${LAUNCHED_GROUP_PID}" =~ ^[0-9]+$ ]] && kill -0 "${LAUNCHED_GROUP_PID}" 2>/dev/null; then
        LAUNCHED_PID_FILE=""
        return 0
      fi
      break
    fi
    if ! process_is_running "${LAUNCHED_PID}"; then
      break
    fi
    sleep 0.01
  done

  rm -f "${pid_file}"
  stop_process_group "${LAUNCHED_PID}" "${LAUNCHED_GROUP_PID}"
  LAUNCHED_PID=""
  LAUNCHED_GROUP_PID=""
  LAUNCHED_PID_FILE=""
  return 1
}

echo "[3/4] Starting backend on http://127.0.0.1:${BACKEND_PORT}"
if ! launch_session "${ROOT}" "${ROOT}/runtime/.backend-session.$$" \
  "${BACKEND_BIN}" "${BACKEND_PORT}"; then
  echo "Could not start the backend session" >&2
  exit 1
fi
BACKEND_PID="${LAUNCHED_PID}"
BACKEND_GROUP_PID="${LAUNCHED_GROUP_PID}"
LAUNCHED_PID=""
LAUNCHED_GROUP_PID=""

echo "[4/4] Waiting for backend health check"
wait_for_backend || exit 1

echo "Starting frontend on the Vite default port"
if ! launch_session "${FRONTEND_DIR}" "${ROOT}/runtime/.frontend-session.$$" \
  env "VITE_BACKEND_URL=http://127.0.0.1:${BACKEND_PORT}" \
      "VITE_BACKEND_PORT=${BACKEND_PORT}" "${VITE_BIN}" --strictPort; then
  echo "Could not start the frontend session" >&2
  exit 1
fi
FRONTEND_PID="${LAUNCHED_PID}"
FRONTEND_GROUP_PID="${LAUNCHED_GROUP_PID}"
LAUNCHED_PID=""
LAUNCHED_GROUP_PID=""

# Do not leave a half-running stack behind. Poll the service leaders explicitly
# because Bash 4.3's `wait -n` cannot restrict itself to a supplied PID list.
while process_is_running "${BACKEND_PID}" && process_is_running "${FRONTEND_PID}"; do
  sleep 0.1
done

child_status=0
if ! process_is_running "${BACKEND_PID}"; then
  if wait "${BACKEND_PID}"; then
    child_status=0
  else
    child_status=$?
  fi
else
  if wait "${FRONTEND_PID}"; then
    child_status=0
  else
    child_status=$?
  fi
fi

exited_services=()
if ! kill -0 "${BACKEND_PID}" 2>/dev/null; then
  exited_services+=("backend")
fi
if ! kill -0 "${FRONTEND_PID}" 2>/dev/null; then
  exited_services+=("frontend")
fi
if (( ${#exited_services[@]} == 0 )); then
  exited_services+=("service")
fi

echo "${exited_services[*]} exited (status ${child_status}); stopping the stack." >&2
if (( child_status == 0 )); then
  exit 1
fi
exit "${child_status}"
