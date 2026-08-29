# Studio AI assistant runbook

The browser never receives a provider key. Studio sends AI requests to the Platform API, and only
that API contacts the selected provider over HTTPS. Migration and worker containers receive the
non-secret feature, quota and retention settings; the API is the only service that receives
`SILICONFLOW_API_KEY` or `DEEPSEEK_API_KEY`.

## Node 1 development

Keep development simple: put the local key in `ai.env` at the repository root. The file is ignored
by Git and does not need `sudo`:

```bash
touch ai.env
chmod 600 ai.env
```

Use this shape:

```dotenv
AI_ENABLED=true
SILICONFLOW_API_KEY=<fill on this machine>
SILICONFLOW_BASE_URL=https://api.siliconflow.cn/v1
SILICONFLOW_MODEL=Qwen/Qwen3.6-35B-A3B
FSD_API_ORIGIN=https://fractal.kevin0412.top
FSD_S3_PUBLIC_ENDPOINT_URL=https://fractal.kevin0412.top
SESSION_COOKIE_SECURE=true
AI_FREE_LIFETIME_LIMIT=10
AI_HISTORY_TTL_DAYS=90
AI_MAX_USER_MESSAGE_CHARS=4000
AI_MAX_OUTPUT_TOKENS=1500
AI_MAX_IMAGE_BYTES=1048576
AI_MAX_CONCURRENT_PER_USER=2
```

For DeepSeek, set `AI_PROVIDER=deepseek` and fill `DEEPSEEK_API_KEY`,
`DEEPSEEK_BASE_URL` and `DEEPSEEK_MODEL` instead. Keep inactive provider keys empty unless they are
temporarily needed for an explicit comparison.

The same ignored file may also hold the optional MiMo evaluation credentials requested for manual
comparison:

```dotenv
MIMO_API_KEY=<optional; evaluation only>
MIMO_BASE_URL=<optional Xiaomi API base URL>
MIMO_MODEL=mimo-v2.5
```

The application and Compose files do not read or inject these `MIMO_*` values. MiMo was retained as
an evaluation option but was not selected for runtime; production still enables exactly one model.

Check that the file exists, is private and that the selected provider key is non-empty without
printing it:

```bash
test "$(stat -c %a ai.env)" = 600
provider="$(awk -F= '/^AI_PROVIDER=/{print $2}' ai.env)"
key_name="$([ "$provider" = deepseek ] && printf DEEPSEEK_API_KEY || printf SILICONFLOW_API_KEY)"
awk -F= -v key="$key_name" 'BEGIN{ok=0} $1==key{ok=length($2)>0} END{exit ok ? 0 : 1}' ai.env
```

Always target the isolated development project explicitly:

```bash
docker compose -p fractal-studio-dev --env-file ai.env -f docker-compose.dev.yml config --quiet
docker compose -p fractal-studio-dev --env-file ai.env -f docker-compose.dev.yml up -d --build
docker compose -p fractal-studio-dev --env-file ai.env -f docker-compose.dev.yml ps
docker compose -p fractal-studio-dev --env-file ai.env -f docker-compose.dev.yml logs --tail=200 api frontend worker
```

`FSD_API_ORIGIN` makes the secure remote domain the canonical development origin. The Compose file
also trusts `http://localhost:3010`, so login/register and CSRF-protected writes work from both review
addresses. Asset URLs use the remote development domain rather than a reviewer's own localhost.
The public review cookie is marked `Secure`; localhost can continue using the per-tab bearer session
returned by login instead of weakening the remote cookie.

The older `fractal-node1-dev` project may still own the same host ports. Cut over once, without
removing its containers or volumes, before starting the new project:

```bash
docker compose -p fractal-node1-dev \
  -f docker-compose.dev.yml -f docker-compose.release.yml stop
docker compose -p fractal-studio-dev --env-file ai.env -f docker-compose.dev.yml up -d --build
```

The first command targets only the legacy development project. It must never be replaced with a
project-less `down`, and it does not touch `fractal-node1-prod`.

Run paid provider checks explicitly; they are not part of deterministic unit tests. The first command
is only a low-cost wire smoke test. Model quality requires an actual Compute preview and its trusted
context:

```bash
docker compose -p fractal-studio-dev --env-file ai.env -f docker-compose.dev.yml \
  exec api uv run --no-sync python scripts/ai-provider-contract.py

docker compose -p fractal-studio-dev --env-file ai.env -f docker-compose.dev.yml \
  exec api uv run --no-sync python scripts/ai-runtime-adapter-contract.py \
  --preview /path/in/container/actual-preview.png \
  --context /path/in/container/trusted-context.json --confirm-paid

docker compose -p fractal-studio-dev --env-file ai.env -f docker-compose.dev.yml \
  exec api uv run --no-sync python scripts/ai-exploration-contract.py \
  --preview /path/in/container/actual-preview.png \
  --context /path/in/container/trusted-context.json --attempts 2

docker compose -p fractal-studio-dev --env-file ai.env -f docker-compose.dev.yml \
  exec api uv run --no-sync python scripts/ai-listing-copy-contract.py \
  --image /path/in/container/actual-listing-preview.png --locale zh \
  --revision-instruction '写得更克制，并按图片真实方位修改'
```

The runtime-adapter contract calls the same `app.ai.provider` implementation as the API. It verifies
real text streaming, usage, a real image with a validated forced tool call, explicit stream closure,
and sanitized connection-failure mapping. Add `--check-invalid-auth` only when intentionally making
one extra provider request with a temporary invalid credential.

Local acceptance endpoints are `http://localhost:3010`, `http://localhost:18100/healthz` and
`http://localhost:18103/compute/v1/health`. The existing development Caddy/FRP route exposes the
same stack at `https://fractal.kevin0412.top`; its `/platform/*` proxy has streaming flush enabled.

Leave the development stack running for review. When a stop is requested, stop only this project:

```bash
docker compose -p fractal-studio-dev --env-file ai.env -f docker-compose.dev.yml stop
```

Do not use a project-less `compose down`, stop production Compute, or run a global Docker prune.

## Production configuration

Production remains host-managed. Add the same AI variables to `/etc/fractal-prod/vps.env`, owned by
the administrator and mode `0600`; never copy the populated file into the repository. The production
Compose template masks the provider keys in PostgreSQL, MinIO, Gateway, migration and workers, then
injects the real value only into the Platform API.

One provider is active at a time. `AI_PROVIDER=siliconflow` (default) uses `SILICONFLOW_*`;
`AI_PROVIDER=deepseek` uses `DEEPSEEK_API_KEY`/`DEEPSEEK_BASE_URL`/`DEEPSEEK_MODEL` and enables
DeepSeek's multimodal streaming and listing copy. The API refuses to boot with `AI_ENABLED=true` and
a missing key for the selected provider. DeepSeek models are reasoning models: hidden
`reasoning_tokens` count inside `max_tokens`, so DeepSeek deployments should keep
`AI_MAX_OUTPUT_TOKENS=2400` (the exploration path has its own higher budget). A DeepSeek model that
answers in text instead of calling the exploration tool is retried once with a regenerated
observation; an invalid tool result stays fail-closed.

Before an approved release, confirm immutable image tags and validate the rendered Compose file:

```bash
test "$(stat -c %a /etc/fractal-prod/vps.env)" = 600
docker compose -p fractal-prod --env-file /etc/fractal-prod/vps.env \
  -f /opt/fractal-prod/docker-compose.vps.yml config --quiet
docker compose -p fractal-prod --env-file /etc/fractal-prod/vps.env \
  -f /opt/fractal-prod/docker-compose.vps.yml up -d --no-build
```

Switching the provider is a Platform-API-only change (workers and migrations never hold a provider
key): update `AI_PROVIDER` and the matching key in `vps.env`, then recreate only the API service with
`docker compose ... up -d --no-build api`. The full release still recreates the whole control plane.

Confirm migration completion, API health, unbuffered text/image streaming, suggestion apply/undo,
history deletion and the free-account 10th/11th request boundary at `https://fractalstudio.cn`.
Rollback Frontend and Platform to the prior immutable SHA images if needed. For an immediate AI-only
rollback, set `AI_ENABLED=false` and update the control plane; Studio and every non-AI route remain
available.

When `AI_ENABLED=true`, a missing key intentionally prevents the API from starting. Migration,
history cleanup and preview workers do not need a provider key. Provider HTTP uses `trust_env=False`,
so application proxy variables are not inherited, and request images stay in memory rather than
being written to PostgreSQL, MinIO, temporary files or logs.
