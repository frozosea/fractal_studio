# Fractal Platform Backend

MVP modular-monolith skeleton. Architecture contract: [platform-backend-spec.md](docs/platform-backend-spec.md).

## Development start

```bash
cp .env.example .env
docker compose -f docker-compose.dev.yml up --build
```

- API: `http://localhost:18000/healthz`, OpenAPI UI: `http://localhost:18000/docs`
- PostgreSQL: `localhost:15432`
- Redis: `localhost:16379`
- MinIO API: `http://localhost:19000`, console: `http://localhost:19001`

`migrate` applies `alembic upgrade head` before API/worker start and watches `migrations/` for
new revisions. API uses Uvicorn reload; worker uses `watchfiles`; source changes apply without
image rebuild. Rebuild only after changing `pyproject.toml` or `uv.lock`.

C++ Compute is outside this Compose project. Run it separately and set `COMPUTE_BASE_URL`; Docker
can reach a host instance through `host.docker.internal`.

## Development user

After Compose starts, create or reuse a local user and persist its cookie jar:

```bash
./scripts/dev-user.sh
```

The script uses real `POST /v1/auth/register` / `POST /v1/auth/login` endpoints, then writes
gitignored local files:

- `scripts/.dev-user.env` — email and generated/provided development password;
- `scripts/.dev-user.cookies` — opaque `fs_session` cookie used by the API.

On first run it registers the user. On later runs it logs in with saved credentials, updates the
cookie jar, and verifies `GET /v1/me`. These files contain local credentials: use only for
development and never commit or share them.

Create a particular local user or point script at another API:

```bash
DEV_USER_EMAIL=alice@example.test DEV_USER_PASSWORD='local-password' ./scripts/dev-user.sh
API_URL=http://localhost:18000 ./scripts/dev-user.sh
```

Use saved session for manual authenticated requests:

```bash
curl --noproxy '*' -fsS \
  -b scripts/.dev-user.cookies \
  http://localhost:18000/v1/me
```
