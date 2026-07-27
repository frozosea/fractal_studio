# Development Guide

Fractal Studio is a single local development stack: Next.js is the browser
surface, Platform owns users and business data, and C++ Compute is private to
the Platform worker. The browser never calls Compute or legacy `/api/*` routes.

## Start the stack

```bash
cp .env.example .env   # optional: defaults work without this file
./dev.sh
```

`dev.sh` runs `docker compose -f docker-compose.dev.yml up --build` and starts:

| Service | Purpose | Default host URL |
| --- | --- | --- |
| `frontend` | Next.js UI and `/platform/*` reverse proxy | http://localhost:3010 |
| `api` | FastAPI Platform API | http://localhost:18100 |
| `worker` | Platform outbox/render worker | internal only |
| `compute` | private C++ Compute v1 | http://localhost:18101 |
| `postgres`, `redis`, `minio` | Platform state and artifacts | ports in `.env.example` |
| `alipay-stub` | local payment callback stub | http://localhost:18102 |

The `migrate` one-shot service runs database migrations before `api` and
`worker`. Stop the stack with:

```bash
./dev.sh --down
```

Use `docker compose -f docker-compose.dev.yml logs -f api worker frontend` for
live logs. Do not run PostgreSQL, Redis, MinIO, or Compute as host processes.

## Browser/API boundary

Browser requests use the same public origin:

```text
Browser -> http://localhost:3010/platform/v1/* -> Next reverse proxy -> Platform
Platform worker -> http://compute:18080/compute/v1/* -> C++ Compute
```

Platform session authentication is the `fs_session` HttpOnly cookie. Mutating
browser requests obtain `GET /platform/v1/auth/csrf-token` and send
`X-CSRF-Token`; idempotent mutations also send `Idempotency-Key`.

## Checks

Start the compose stack first for the browser test, then run:

```bash
cd frontend
pnpm install --frozen-lockfile
pnpm type-check
pnpm build
pnpm test:e2e

cd ../platform-backend
uv run pytest -q tests/unit/test_studio.py tests/unit/test_render_mapper.py
./scripts/e2e-real-compute.sh
```

`frontend/tests/e2e/platform-smoke.spec.ts` proves a real browser registration,
cookie session, Chinese/English Studio routing, map/Julia/transition/formula
preview requests, dynamic coloring and the browser-to-Platform boundary. The Platform real
Compute test covers preview plus image, video, HS mesh and transition mesh
artifacts.

## Environment

Copy `.env.example` only to alter collision-safe host ports or local secrets.
Never commit `.env`, Alipay keys, session secrets, database dumps, or MinIO
data. Production requires HTTPS origins and real Alipay credentials; the
Compose development stack deliberately uses the payment stub.

### HTTPS reverse proxy

When the stack is reached through a domain, configure both the browser origin
and the public object-storage origin in the root `.env`:

```dotenv
FSD_API_ORIGIN=https://ai.example.com
FSD_S3_PUBLIC_ENDPOINT_URL=https://assets.ai.example.com
```

The first hostname proxies to the frontend on port `3010`. The second hostname
must proxy to MinIO's S3 port `19010`; preserve the incoming `Host` header or
S3 signatures will fail. For nginx, the relevant server is:

```nginx
server {
    listen 443 ssl http2;
    server_name assets.ai.example.com;

    location / {
        proxy_set_header Host $http_host;
        proxy_set_header X-Forwarded-Proto $scheme;
        proxy_pass http://127.0.0.1:19010;
    }
}
```

Use a dedicated storage hostname rather than a path such as `/minio`: the URL
path is part of the S3 signature. Restart `api` after changing the public S3
origin; newly requested preview and download URLs will then use the domain.

## Legacy Compute API

The repository retains historical C++ `/api/*` material for migration and
engineering reference. It is not part of the browser product path. The current
worker contract is [Platform Compute spec](../platform-backend/docs/compute-spec.md)
and [OpenAPI](../platform-backend/docs/compute-openapi.yaml).
