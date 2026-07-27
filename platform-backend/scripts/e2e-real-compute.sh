#!/usr/bin/env bash
set -euo pipefail

# T15 Platform proof: C++ Compute v1, then black-box Platform render and ingest.
PLATFORM_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STUDIO_DIR="$(cd "$PLATFORM_DIR/.." && pwd)"
PROJECT_NAME="${E2E_COMPUTE_PROJECT:-fractal-compute-platform-e2e}"
PORT="${E2E_REAL_COMPUTE_PORT:-28080}"
KEY="${E2E_REAL_COMPUTE_SERVICE_KEY:-e2e-real-compute-key-change-me}"

compute_compose() {
  FSD_COMPUTE_SERVICE_KEY="$KEY" COMPUTE_PORT="$PORT" \
    docker compose -p "$PROJECT_NAME" -f "$STUDIO_DIR/docker-compose.dev.yml" "$@"
}

cleanup() {
  if [[ "${E2E_KEEP_STACK:-0}" != "1" ]]; then
    compute_compose down --volumes --remove-orphans
  fi
}
trap cleanup EXIT

compute_compose down --volumes --remove-orphans
compute_compose up --build --detach compute
for _ in {1..90}; do
  if curl --noproxy '*' -fsS "http://127.0.0.1:${PORT}/compute/v1/health" >/dev/null; then
    break
  fi
  sleep 1
done
curl --noproxy '*' -fsS "http://127.0.0.1:${PORT}/compute/v1/health" >/dev/null

cd "$PLATFORM_DIR"
E2E_COMPUTE_BASE_URL="http://host.docker.internal:${PORT}" \
E2E_COMPUTE_SERVICE_KEY="$KEY" \
E2E_REAL_COMPUTE_URL="http://127.0.0.1:${PORT}" \
E2E_REAL_COMPUTE_SERVICE_KEY="$KEY" \
E2E_REAL_COMPUTE_PLATFORM=1 \
E2E_TEST_FILES="tests/e2e/test_compute_production_contract.py tests/e2e/test_real_compute_platform_e2e.py" \
  ./scripts/e2e.sh
