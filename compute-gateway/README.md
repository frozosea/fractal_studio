# Fractal Compute Gateway

Private stateful router for multiple C++ Compute nodes. It keeps each durable
run sticky to its original node while exposing the existing `/compute/v1/*`
contract to Platform.

Design and API contract: [`../docs/compute-gateway-mvp/README.md`](../docs/compute-gateway-mvp/README.md).

## Local development

```bash
cp .env.example .env
uv sync --all-groups
alembic -c alembic.ini upgrade head
uvicorn app.main:app --host 0.0.0.0 --port 8080
```

Use root Compose for the complete local topology. It starts `compute-gateway-db`,
applies Gateway migrations, bootstraps the local C++ Compute node, and points
Platform's `COMPUTE_BASE_URL` at `http://compute-gateway:8080`.

```bash
docker compose -f ../docker-compose.dev.yml up -d --build
```

The production service is private-only. The root Compose port `18103` exists
for local development and diagnostics.

## Verification

```bash
uv run ruff check .
GATEWAY_TEST_DATABASE_URL=postgresql+asyncpg://gateway:gateway_dev_password@localhost:25443/compute_gateway_test \
  DATABASE_URL=$GATEWAY_TEST_DATABASE_URL \
  COMPUTE_GATEWAY_SERVICE_KEY=test-gateway-key-1234 \
  COMPUTE_GATEWAY_ADMIN_KEY=test-admin-key-123456 \
  COMPUTE_UPSTREAM_SERVICE_KEY=test-upstream-key-123 \
  uv run pytest -q
```

`compute_gateway_test` must be a disposable PostgreSQL database migrated with
this service's Alembic revision; tests refuse to run against another DB name.
