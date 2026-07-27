#!/usr/bin/env bash
set -euo pipefail

# One isolated T14 gate. Its project name and host ports never touch a developer stack.
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPOSE_FILE="$ROOT_DIR/docker-compose.dev.yml"
PROJECT_NAME="${E2E_COMPOSE_PROJECT:-fractal-platform-e2e}"

export API_PORT="${E2E_API_PORT:-28000}"
export POSTGRES_PORT="${E2E_POSTGRES_PORT:-25432}"
export REDIS_PORT="${E2E_REDIS_PORT:-26379}"
export MINIO_PORT="${E2E_MINIO_PORT:-29000}"
export MINIO_CONSOLE_PORT="${E2E_MINIO_CONSOLE_PORT:-29001}"
export E2E_COMPUTE_STUB_PORT="${E2E_COMPUTE_STUB_PORT:-28081}"
export E2E_ALIPAY_STUB_PORT="${E2E_ALIPAY_STUB_PORT:-28082}"
export COMPUTE_BASE_URL="${E2E_COMPUTE_BASE_URL:-http://compute-stub:8000}"
export COMPUTE_SERVICE_KEY="${E2E_COMPUTE_SERVICE_KEY:-test-compute-key}"
export ALIPAY_STUB_MODE=true
export ALIPAY_APP_ID=dev-stub
export ALIPAY_SELLER_ID=dev-stub
export ALIPAY_STUB_PUBLIC_KEY_URL="http://alipay-stub:8000/test/public-key"
export ALIPAY_GATEWAY_URL="http://alipay-stub:8000/gateway.do"
export PAYMENT_RECONCILE_DELAY_SECONDS=1
export PAYMENT_RECONCILE_PENDING_SECONDS=5
export PAYMENT_RECONCILE_SWEEP_SECONDS=30
export OUTBOX_POLL_INTERVAL_SECONDS=0.2
export OUTBOX_SCHEDULE_INTERVAL_SECONDS=1
export PREVIEW_RATE_LIMIT_PER_MINUTE=30
export E2E_FINANCE_EMAIL="${E2E_FINANCE_EMAIL:-finance-operator@e2e.invalid}"
export E2E_FINANCE_PASSWORD="${E2E_FINANCE_PASSWORD:-e2e-finance-password-01}"
export E2E_DISABLED_EMAIL="${E2E_DISABLED_EMAIL:-disabled-user@e2e.invalid}"
export E2E_DISABLED_PASSWORD="${E2E_DISABLED_PASSWORD:-e2e-disabled-password-01}"
export E2E_API_URL="http://127.0.0.1:${API_PORT}"
export E2E_ALIPAY_STUB_URL="http://127.0.0.1:${E2E_ALIPAY_STUB_PORT}"

compose() {
  docker compose -p "$PROJECT_NAME" -f "$COMPOSE_FILE" --profile e2e "$@"
}

cleanup() {
  local exit_status=$?
  if [[ "$exit_status" -ne 0 ]]; then
    compose logs --tail=200 api worker e2e-fixtures compute-stub alipay-stub >&2 || true
  fi
  if [[ "${E2E_KEEP_STACK:-0}" != "1" ]]; then
    compose down --volumes --remove-orphans
  fi
}
trap cleanup EXIT

compose down --volumes --remove-orphans
compose up --build --detach
compose wait e2e-fixtures

curl --noproxy '*' -fsS "$E2E_API_URL/healthz" >/dev/null
curl --noproxy '*' -fsS "$E2E_API_URL/v1/explore?sort=newest&limit=24" >/dev/null

cd "$ROOT_DIR"
uv run pytest -q \
  ${E2E_TEST_FILES:-tests/e2e/test_full_user_journey.py tests/e2e/test_mvp_happy_path.py tests/e2e/test_failure_recovery.py tests/e2e/test_security_boundaries.py tests/e2e/test_api_contract_matrix.py}
