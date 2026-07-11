#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BACKEND_BUILD_DIR="${ROOT}/runtime/build"
BACKEND_BIN="${BACKEND_BUILD_DIR}/fractal_studio_backend"
FRONTEND_DIR="${ROOT}/frontend"
BACKEND_PID=""

cleanup() {
  if [[ -n "${BACKEND_PID}" ]] && kill -0 "${BACKEND_PID}" 2>/dev/null; then
    kill "${BACKEND_PID}" 2>/dev/null || true
    wait "${BACKEND_PID}" 2>/dev/null || true
  fi
}

trap cleanup EXIT INT TERM

if ! command -v cmake >/dev/null 2>&1; then
  echo "cmake not found" >&2
  exit 1
fi

if ! command -v npm >/dev/null 2>&1; then
  echo "npm not found" >&2
  exit 1
fi

echo "[1/4] Building backend"
cmake -S "${ROOT}/backend" -B "${BACKEND_BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${BACKEND_BUILD_DIR}" -j

if [[ ! -x "${BACKEND_BIN}" ]]; then
  echo "Backend binary not found: ${BACKEND_BIN}" >&2
  exit 1
fi

if [[ ! -d "${FRONTEND_DIR}/node_modules" ]]; then
  echo "[2/4] Installing frontend dependencies"
  (cd "${FRONTEND_DIR}" && npm ci)
fi

echo "[3/4] Starting backend on http://127.0.0.1:18080"
"${BACKEND_BIN}" 18080 &
BACKEND_PID=$!

if command -v curl >/dev/null 2>&1; then
  echo "[4/4] Waiting for backend health check"
  ready=0
  for _ in {1..60}; do
    if curl -fsS "http://127.0.0.1:18080/api/system/check" >/dev/null 2>&1; then
      ready=1
      break
    fi
    sleep 0.5
  done
  if [[ "${ready}" -ne 1 ]]; then
    echo "Backend did not become ready on port 18080" >&2
    exit 1
  fi
else
  echo "[4/4] curl not found; waiting briefly before starting frontend"
  sleep 2
fi

echo "Starting frontend on the Vite default port"
cd "${FRONTEND_DIR}"
npm run dev
