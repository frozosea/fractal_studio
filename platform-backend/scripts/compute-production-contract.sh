#!/usr/bin/env bash
set -euo pipefail

# T15: real C++ Compute, private network contract. No Python Compute stub is used.
PLATFORM_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STUDIO_DIR="$(cd "$PLATFORM_DIR/.." && pwd)"
PROJECT_NAME="${E2E_COMPUTE_PROJECT:-fractal-compute-contract}"
PORT="${E2E_REAL_COMPUTE_PORT:-28080}"
KEY="${E2E_REAL_COMPUTE_SERVICE_KEY:-e2e-real-compute-key-change-me}"

compose() {
  FSD_COMPUTE_SERVICE_KEY="$KEY" COMPUTE_PORT="$PORT" \
    docker compose -p "$PROJECT_NAME" -f "$STUDIO_DIR/docker-compose.dev.yml" "$@"
}

cleanup() {
  if [[ "${E2E_KEEP_STACK:-0}" != "1" ]]; then
    compose down --volumes --remove-orphans
  fi
}
trap cleanup EXIT

compose down --volumes --remove-orphans
compose up --build --detach compute

for _ in {1..90}; do
  if curl --noproxy '*' -fsS "http://127.0.0.1:${PORT}/compute/v1/health" >/dev/null; then
    break
  fi
  sleep 1
done
curl --noproxy '*' -fsS "http://127.0.0.1:${PORT}/compute/v1/health" >/dev/null

cd "$PLATFORM_DIR"
E2E_REAL_COMPUTE_URL="http://127.0.0.1:${PORT}" \
E2E_REAL_COMPUTE_SERVICE_KEY="$KEY" \
  uv run pytest -q tests/e2e/test_compute_production_contract.py
