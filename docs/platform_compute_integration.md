# Platform–Gateway–Compute Integration / 服务端计算对接

本文描述当前生产调用链。Platform 使用统一 `/compute/v1/*` 合同访问 Compute Gateway，Gateway
再把请求路由到 0..N 个私有 C++ Compute 节点。早期 Platform 直连单节点 `/api/*` 的适配记录已
归档在 [compute_api_contract_implementation.md](compute_api_contract_implementation.md)，不再是
当前实现。

```text
Browser
  -> Platform public /v1/*
      -> ComputeClient
          -> Compute Gateway /compute/v1/*
              -> selected private C++ Compute /compute/v1/*
```

## 1. Trust boundaries and configuration

Platform production configuration:

```dotenv
COMPUTE_BASE_URL=http://compute-gateway:8080
COMPUTE_SERVICE_KEY=<platform-to-gateway-service-key>
```

Gateway separately receives:

```dotenv
COMPUTE_GATEWAY_SERVICE_KEY=<same-platform-to-gateway-key>
COMPUTE_GATEWAY_ADMIN_KEY=<separate-admin-key>
COMPUTE_UPSTREAM_SERVICE_KEY=<gateway-to-compute-key>
COMPUTE_GATEWAY_BOOTSTRAP_NODES_JSON=<complete-0..N-node-array>
```

Every C++ node receives the same current upstream key as
`FSD_COMPUTE_SERVICE_KEY`. Browser code receives none of these values. Platform worker does not receive
the upstream Compute key, and only the Platform API process may receive the optional Gateway admin key
for node monitoring.

Production HTTP clients use `trust_env=False`, bounded connect/read timeouts and safe error mapping;
host proxy variables cannot silently redirect private service traffic.

## 2. Current private routes

`platform-backend/app/infrastructure/compute/compute_client.py` uses:

| Method | Route | Purpose |
|---|---|---|
| `GET` | `/compute/v1/capabilities` | aggregate healthy compatible capabilities and admission probe |
| `POST` | `/compute/v1/previews` | bounded non-persistent RGBA8 preview |
| `POST` | `/compute/v1/runs` | idempotent durable run creation |
| `GET` | `/compute/v1/runs/{gatewayRunId}` | status and progress |
| `GET` | `/compute/v1/runs/{gatewayRunId}/manifest` | terminal artifact metadata and SHA-256 |
| `POST` | `/compute/v1/runs/{gatewayRunId}/cancel` | cancellation intent |
| `GET` | `/compute/v1/artifacts?artifactId=...` | private streaming artifact read |

The Gateway returns its own run and artifact IDs. Platform treats them as opaque; it never constructs a
node URL or depends on a node-local run ID.

## 3. Canonical mapping boundary

`platform-backend/app/studio/compute_request_mapper.py` is the only mapping from product recipes and
output specs to Compute envelopes. Browser JSON is never forwarded directly.

Common envelope:

```json
{
  "schemaVersion": 1,
  "kind": "map_image",
  "idempotencyKey": "stable-job-uuid-for-durable-only",
  "payload": {}
}
```

`map_preview_v1()`:

- chooses `map_image` or `transition_image` from canonical transition mode;
- maps product engine/scalar names to capability names;
- forwards only supported formula, sequence, coloring and transition fields;
- clamps preview iterations, pairwise cost and viewport bounds without mutating the saved recipe;
- omits an idempotency key because preview is non-persistent.

`map_durable_v1()`:

- uses the Platform render-job UUID as the stable Compute idempotency key;
- supports current product image/video/HS mesh/transition mesh output mappings;
- rejects unsupported output/color combinations and out-of-range workload before persistence;
- returns both the private route and immutable request body, which Platform saves before worker
  submission.

The canonical recipe remains the product fact. AI patches and browser edits must pass the same recipe
validation and mapper budgets before they can occupy shared Compute capacity.

## 4. Interactive preview flow

Platform exposes both a synchronous preview endpoint and Redis-backed latest-wins preview jobs.

Synchronous path:

1. validate width, height and pixel count;
2. enforce per-user preview rate limit;
3. map a bounded Compute v1 preview envelope;
4. call Gateway with the configured short preview timeout;
5. require `X-FSD-Width`, `X-FSD-Height`, `X-FSD-Pixel-Format: rgba8` and exact byte length;
6. encode RGBA8 to PNG inside Platform.

Asynchronous path additionally checks Gateway capabilities before queue admission, stores bounded
latest-wins work in Redis, and lets `preview-worker` claim/retry it. The worker reapplies current bounds
at execution so a queued request from an older release cannot bypass new limits.

No healthy compatible slot, timeout or Gateway unavailability maps to Platform HTTP 503 with stable
detail `COMPUTE_CAPACITY_EXHAUSTED`. Invalid upstream frames map to 502; rate-limit and Redis failures
remain distinct Platform errors.

## 5. Durable render and quota state machine

Creation is transactional:

```text
claim browser idempotency key
  -> load owned immutable recipe
  -> map and bound Compute v1 request
  -> call capabilities as fail-closed admission check
  -> create Platform render_job
  -> reserve quota atomically
  -> append render.created.v1 outbox event
  -> complete browser idempotency response
```

If capacity is unavailable at admission, Platform returns 503 before creating the render job, outbox
event or quota reservation.

The outbox render worker then follows:

```text
render.created.v1
  -> persist submitting state
  -> POST saved route/body to Gateway
  -> persist opaque Gateway compute_run_id
  -> schedule render.poll.v1

render.poll.v1
  -> GET Gateway run
  -> queued/running: update progress and reschedule
  -> completed: GET manifest, select one expected artifact, ingest
  -> failed/cancelled: terminalize and apply quota policy

render.cancel_requested.v1
  -> POST Gateway cancellation
  -> continue polling until Compute reports terminal truth
```

Outbox delivery is at least once. Rows are claimed with `FOR UPDATE SKIP LOCKED`, and the stable
Platform job ID plus the saved immutable body plus Gateway idempotency ensures retries do not
duplicate runs or quota. Network calls happen outside long-held PostgreSQL locks. The sticky node's
`compute_node_id` and the opaque Gateway `compute_run_id` are both persisted so every follow-up
operation targets the same node.

Gateway persists the selected node before forwarding. Status, cancellation, manifest and artifact reads
remain sticky to that node even if other nodes become healthier later.

## 6. Artifact ingestion

Platform never gives a Compute artifact URL to the browser. It streams bytes from Gateway and enforces:

- artifact ID contains no slash, backslash or traversal token;
- manifest media type matches the requested product output;
- declared size is positive and below Platform media limits;
- streamed byte count equals manifest size;
- calculated SHA-256 equals manifest SHA-256;
- only the selected expected artifact is ingested;
- manifest hardware evidence (`kernelReported`, `actualEngine`, `hardwareClass`) is recorded as
  provenance without trusting the requested `engine` parameter;
- final upload uses server-side encryption in VPS MinIO.

After upload, Platform owns the commercial asset and issues entitlement-checked, short-lived S3 signed
URLs. The Caddy route preserves `/fractal-platform/*` because the full path participates in SigV4.

## 7. Stable error mapping

`ComputeClient` never returns raw upstream bodies to product routes:

| Upstream condition | Platform adapter code |
|---|---|
| 401/403 | `compute_auth_failed` |
| 404 | `compute_run_not_found` |
| 409 | `compute_conflict` |
| 503 + `COMPUTE_CAPACITY_EXHAUSTED` | `compute_capacity_exhausted` |
| timeout | `compute_timeout` |
| other 5xx/network | `compute_unavailable` |
| invalid JSON/frame/manifest | `compute_invalid_*` |
| other contract rejection | sanitized `compute_<code>` or `compute_rejected` |

Platform product services decide whether these become 422, 502 or 503. Capacity, timeout and unavailable
conditions are deliberately fail closed for new preview/render admission.

## 8. Multi-node semantics are Gateway-owned

Platform has one `COMPUTE_BASE_URL` because that URL is the Gateway, not one physical Compute node.
Adding Node 3 changes the Gateway bootstrap inventory and private network only; it does not add another
Platform URL or code branch.

Gateway handles:

- arbitrary node count and stable node identity;
- health/capability refresh and automatic offline recovery;
- CPU/GPU durable and preview slot accounting;
- least-loaded compatible selection with database locking;
- opaque run/artifact ID rewriting and original-node affinity;
- zero-node health with explicit capacity failures.

Platform handles:

- authentication, rate limit and quota admission;
- product recipe/output validation;
- job/outbox lifecycle and user-visible error semantics;
- artifact integrity, encryption, ownership and download authorization.

Do not duplicate Gateway node selection in Platform or infer scheduling from GPU names.

## 9. Verification

Relevant deterministic tests:

```bash
cd platform-backend
uv run pytest -q tests/unit/test_studio.py tests/unit/test_render_mapper.py

cd ../compute-gateway
uv run pytest -q
```

The Gateway test database must be disposable and explicitly named/configured as described in
[compute-gateway/README.md](../compute-gateway/README.md). Full real-service checks are documented in
[testing.md](testing.md) and [ops/production/INSTALL.md](../ops/production/INSTALL.md).

Production acceptance must include a real preview and durable render through Platform -> Gateway ->
Compute, manifest/SHA verification, VPS MinIO ingestion and final authorized download. With multiple
nodes, verify distribution across compatible nodes and affinity for every follow-up operation. Fault
tests require a separate approved maintenance window.
