# T01 — Application Foundation, Development Stack, and Observability

## Task description

Build the runnable modular-monolith foundation: API, worker, PostgreSQL, Redis, and MinIO. Establish configuration, async database lifecycle, request context, logging, health checks, and safe local operational defaults. This task deliberately contains no product business state.

## Work scope

- pyproject.toml, uv.lock, Dockerfile, .env.example, .gitignore, alembic.ini
- docker-compose.dev.yml, scripts/dev-migrate-watch.sh
- app/main.py; app/core/config.py, db.py, request_context.py, logging.py, access_middleware.py
- tests/e2e/test_foundation.py

## Goal

Provide one reproducible development environment with hot reload, migrations before API/worker startup, UTC correlated logs, and no secret leakage.

## Acceptance criteria

- docker compose -f docker-compose.dev.yml up --build starts migrate, API, worker, PostgreSQL, Redis, and MinIO; no host database or cache is required.
- API replies with X-Request-ID; request logs have exactly timestamp_utc | request_id | idempotency_key | user_id | level | message in that order.
- One shared HTTP boundary enforces JSON success envelopes { "data": T }, collection envelopes with page.nextCursor, and the documented error envelope { "error": { "code", "message", "details" } }. It maps every baseline error code: 401 unauthenticated, 403 forbidden, 404 not_found, 409 invalid_state/idempotency_conflict, 413 payload_too_large, 422 validation_error, 429 quota_exceeded, 502 compute_error, and 503 payment_unavailable.
- Invalid or absent request IDs are replaced by UUIDv7. /readyz is not successful before migrations and dependencies are ready.
- Production secrets have no defaults and .env, keys, database files, auth files, logs, .venv, node_modules, and .codex are ignored.

## Specification source

Purpose; Stack Map; Runtime Architecture; Logging And Request Correlation Policy; Transport Rules; Final Source Layout; MVP Technical Limits.

## Dependencies

- Python 3.12, FastAPI/Uvicorn, Pydantic v2, SQLAlchemy async, asyncpg, Alembic, Docker Compose.
- PostgreSQL, Redis, and MinIO run only as Compose services.
- No preceding task. T02 and all product tasks depend on this task.

## Test plan

~~~bash
docker compose -f docker-compose.dev.yml up --build -d
curl --noproxy '*' -sS -f -D /tmp/platform-headers http://localhost:8000/healthz
curl --noproxy '*' -sS -f -D - -H 'X-Request-ID: malformed' http://localhost:8000/readyz
curl --noproxy '*' -sS -o /tmp/error.json -w '%{http_code}\n' http://localhost:8000/v1/me
docker compose -f docker-compose.dev.yml logs api worker --tail=100
~~~

Assert HTTP 200 for health/readiness, a newly generated valid X-Request-ID, healthy Compose dependencies, and no fs_session, service key, S3 key, or signed URL in logs. Assert /v1/me returns the exact 401 error envelope with code unauthenticated.

## Implementation plan

1. Add typed settings; read secrets only from environment and redact them everywhere.
2. Add async engine/session lifecycle, startup/shutdown hooks, /healthz, and /readyz.
3. Add request middleware that accepts only a trusted valid edge ID, otherwise creates UUIDv7 and stores it in request context.
4. Configure exact local pipe rendering plus production JSON structured logging.
5. Add global response serialization and exception translation; individual routers may supply data but cannot bypass the transport envelope or baseline code mapping.
6. Define Compose services, health checks, bind mounts, migration wait, and API/worker reload.
7. Complete repository ignore rules.
