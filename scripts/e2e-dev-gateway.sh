#!/usr/bin/env bash
set -euo pipefail

# Full user journey against this repository's running dev topology:
# Platform -> Gateway -> two real C++ Compute nodes. Payment edge uses Alipay stub.
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPOSE=(
  docker compose
  --project-name fractal-studio-dev
)
if [[ -f "$ROOT_DIR/.env" ]]; then
  COMPOSE+=(--env-file "$ROOT_DIR/.env")
fi
COMPOSE+=(
  -f "$ROOT_DIR/docker-compose.dev.yml"
  -f "$ROOT_DIR/docker-compose.e2e.yml"
)

container_env() {
  local service="$1"
  local variable="$2"
  local container
  container="$("${COMPOSE[@]}" ps -q "$service")"
  if [[ -z "$container" ]]; then
    printf 'service %s is not running\n' "$service" >&2
    return 1
  fi
  docker inspect --format '{{range .Config.Env}}{{println .}}{{end}}' "$container" |
    awk -F= -v key="$variable" '$1 == key {print substr($0, length(key) + 2); exit}'
}

if [[ "${E2E_BUILD:-0}" == "1" ]]; then
  "${COMPOSE[@]}" up --build --detach
else
  "${COMPOSE[@]}" up --detach
fi
"${COMPOSE[@]}" wait e2e-fixtures

export E2E_API_URL="${E2E_API_URL:-http://127.0.0.1:18100}"
export E2E_DATABASE_URL="${E2E_DATABASE_URL:-postgresql+asyncpg://fractal:fractal_dev_password@127.0.0.1:15442/fractal_platform}"
export E2E_ALIPAY_STUB_URL="${E2E_ALIPAY_STUB_URL:-http://127.0.0.1:18102}"
export E2E_COMPUTE_AVAILABLE=1
export E2E_PLATFORM_WORKER=1
export E2E_REAL_COMPUTE_PLATFORM=1
export E2E_FINANCE_EMAIL="${E2E_FINANCE_EMAIL:-finance-operator@e2e.invalid}"
export E2E_FINANCE_PASSWORD="${E2E_FINANCE_PASSWORD:-e2e-finance-password-01}"
export E2E_DISABLED_EMAIL="${E2E_DISABLED_EMAIL:-disabled-user@e2e.invalid}"
export E2E_DISABLED_PASSWORD="${E2E_DISABLED_PASSWORD:-e2e-disabled-password-01}"

export GATEWAY_LIVE_URL="${GATEWAY_LIVE_URL:-http://127.0.0.1:18103}"
if [[ -z "${GATEWAY_LIVE_SERVICE_KEY:-}" ]]; then
  GATEWAY_LIVE_SERVICE_KEY="$(container_env compute-gateway COMPUTE_GATEWAY_SERVICE_KEY)"
fi
if [[ -z "${GATEWAY_LIVE_DATABASE_URL:-}" ]]; then
  GATEWAY_LIVE_DATABASE_URL="$(container_env compute-gateway DATABASE_URL)"
  GATEWAY_LIVE_DATABASE_URL="${GATEWAY_LIVE_DATABASE_URL/postgresql+asyncpg:/postgresql:}"
  GATEWAY_LIVE_DATABASE_URL="${GATEWAY_LIVE_DATABASE_URL/@compute-gateway-db:5432/@127.0.0.1:25443}"
fi
if [[ -z "$GATEWAY_LIVE_SERVICE_KEY" || -z "$GATEWAY_LIVE_DATABASE_URL" ]]; then
  printf 'running Gateway configuration is incomplete\n' >&2
  exit 1
fi
export GATEWAY_LIVE_SERVICE_KEY GATEWAY_LIVE_DATABASE_URL

cd "$ROOT_DIR/platform-backend"
# These two tests intentionally require the old programmable Compute stub to
# inject a synthetic transient failure.  The real C++ nodes reject that fake
# variant; real compute coverage is supplied by test_real_compute_platform_e2e.
uv run pytest -q tests/e2e \
  --ignore=tests/e2e/test_compute_production_contract.py \
  --ignore=tests/e2e/test_failure_recovery.py \
  --ignore=tests/e2e/test_render_jobs.py
cd "$ROOT_DIR/compute-gateway"
uv run pytest -q tests/test_live_distribution.py
