# Compute Gateway MVP — Development Specification

**Status:** implementation-ready MVP specification
**Language:** English
**Service name:** `compute-gateway`
**API prefix:** `/compute/v1`
**Primary goal:** route one Platform Compute contract safely across multiple stateful Compute nodes.

## 1. Problem and decision

The current C++ Compute service owns run state locally: a run, its cancellation
token, SQLite idempotency record, and artifacts exist on the specific machine
that accepted `POST /compute/v1/runs`. A generic L4/L7 round-robin load
balancer is therefore incorrect: later status, cancel, manifest, or artifact
requests may reach another node and return `404`; retries can create duplicate
runs on different nodes.

`compute-gateway` is a separate private Docker service and the single Compute
endpoint used by Platform. It is a stateful routing control plane, not a
generic reverse proxy.

```mermaid
flowchart LR
  PW[Platform outbox worker] -->|Compute v1 + gateway key| GW[compute-gateway]
  GW -->|private Compute v1 + upstream key| A[Compute node A\nGPU]
  GW -->|private Compute v1 + upstream key| B[Compute node B\nGPU]
  GW -->|private Compute v1 + upstream key| C[Compute node C\nCPU]
  GW <--> DB[(Gateway PostgreSQL)]
  A --> RA[(local SQLite + runtime)]
  B --> RB[(local SQLite + runtime)]
  C --> RC[(local SQLite + runtime)]
```

Platform sees one opaque `computeRunId`. Gateway records which node owns that
run and always forwards stateful requests to that same node.

## 2. Scope

### Included in MVP

- Multiple C++ Compute nodes, each with its own local SQLite and runtime files.
- Capability-aware and capacity-aware node selection for previews and durable runs.
- Durable global run-to-node routing, idempotent create, polling, cancellation,
  manifest reads, and artifact streaming.
- Periodic node health/capability probing and a manual drain state.
- Dedicated Gateway PostgreSQL database and Docker Compose service definition.
- API compatibility with Platform's current `ComputeClient`: no Platform code
  change is required except `COMPUTE_BASE_URL` and service-key configuration.

### Explicit MVP non-goals

- Migrating an active run between nodes.
- Sharing Compute SQLite files, runtime directories, or GPU memory across hosts.
- Cross-region routing, autoscaling, billing, per-user scheduling, or queue
  priority classes.
- Gateway high availability. MVP runs exactly one Gateway replica; durable route
  state is already stored in PostgreSQL so active/passive HA can be added later.
- Exposing Gateway or Compute to browsers or the public Internet.
- Automatic retry of a durable run on another node after the selected node has
  accepted it. A retry could duplicate expensive work; failure is explicit.

## 3. Terminology and invariants

| Term | Meaning |
|---|---|
| **Gateway run ID** | UUID issued by Gateway and returned as `computeRunId` to Platform. |
| **Node run ID** | Local opaque ID returned by the selected C++ Compute node. Never exposed to Platform. |
| **Route** | Durable mapping `gateway_run_id -> compute_node_id + node_run_id`. |
| **Reservation** | Capacity held on a node for a non-terminal durable gateway run. |
| **Node** | One C++ Compute HTTP process with one local runtime directory and SQLite DB. |

Required invariants:

1. One `idempotencyKey` plus identical request body maps to exactly one
   Gateway run and one selected node.
2. The same key with a different normalized body returns `409
   IDEMPOTENCY_CONFLICT`.
3. A durable route is assigned before Gateway calls the node. All retry attempts
   use that exact node and original request body.
4. All run, cancel, manifest, and artifact requests resolve the route before
   contacting a node.
5. A node in `draining`, `offline`, or `disabled` state never receives a new
   preview or durable run.
6. Terminal states (`completed`, `failed`, `cancelled`, `node_lost`) release
   capacity exactly once.
7. Gateway never returns a node URL, node run ID, local path, upstream key, or
   Compute artifact ID to Platform or a browser.

## 4. Service boundary and Docker deployment

### 4.1 Network boundary

```text
Platform API / Platform worker
        |
        | private Docker network; COMPUTE_GATEWAY_SERVICE_KEY
        v
compute-gateway:8080
        |
        | private node network; COMPUTE_UPSTREAM_SERVICE_KEY
        v
Compute nodes:18080
```

- Only `GET /compute/v1/health` is unauthenticated, for private liveness probes.
- Every other Gateway endpoint requires `Authorization: Bearer
  <COMPUTE_GATEWAY_SERVICE_KEY>`.
- Gateway authenticates to Compute nodes with a separate
  `COMPUTE_UPSTREAM_SERVICE_KEY`.
- Production deploys must place Gateway and all Compute nodes on private
  networks. Do not publish Compute ports; only Platform ingress is public.

### 4.2 Required environment

```dotenv
PORT=8080
DATABASE_URL=postgresql+asyncpg://gateway:change-me@gateway-db:5432/compute_gateway

# Incoming Platform -> Gateway authentication
COMPUTE_GATEWAY_SERVICE_KEY=replace-with-32-byte-secret

# Outgoing Gateway -> C++ Compute authentication
COMPUTE_UPSTREAM_SERVICE_KEY=replace-with-different-32-byte-secret

NODE_PROBE_INTERVAL_SECONDS=5
NODE_CAPABILITIES_REFRESH_SECONDS=60
NODE_OFFLINE_AFTER_SECONDS=15
CREATE_CONNECT_TIMEOUT_SECONDS=2
CREATE_READ_TIMEOUT_SECONDS=15
PREVIEW_TIMEOUT_SECONDS=8
ARTIFACT_READ_TIMEOUT_SECONDS=300
```

### 4.3 Compose topology

This is the target addition to the root `docker-compose.dev.yml`. The Gateway
database is separate from Platform PostgreSQL so Gateway migrations and route
state have an independent lifecycle. MVP uses one Gateway replica.

```yaml
  compute-gateway-db:
    image: postgres:16-alpine
    environment:
      POSTGRES_DB: compute_gateway
      POSTGRES_USER: gateway
      POSTGRES_PASSWORD: ${COMPUTE_GATEWAY_DB_PASSWORD:-gateway_dev_password}
    volumes: [compute-gateway-postgres:/var/lib/postgresql/data]
    healthcheck:
      test: ["CMD-SHELL", "pg_isready -U gateway -d compute_gateway"]
      interval: 2s
      timeout: 2s
      retries: 30

  compute-gateway:
    build: { context: ./compute-gateway }
    environment:
      DATABASE_URL: postgresql+asyncpg://gateway:${COMPUTE_GATEWAY_DB_PASSWORD:-gateway_dev_password}@compute-gateway-db:5432/compute_gateway
      COMPUTE_GATEWAY_SERVICE_KEY: ${COMPUTE_GATEWAY_SERVICE_KEY:-dev-gateway-key-change-me}
      COMPUTE_UPSTREAM_SERVICE_KEY: ${FSD_COMPUTE_SERVICE_KEY:-dev-compute-key-change-me}
    depends_on:
      compute-gateway-db: { condition: service_healthy }
      compute-a: { condition: service_healthy }
    expose: ["8080"]

  # Platform api and worker
  # COMPUTE_BASE_URL: http://compute-gateway:8080
  # COMPUTE_SERVICE_KEY: ${COMPUTE_GATEWAY_SERVICE_KEY:-dev-gateway-key-change-me}

volumes:
  compute-gateway-postgres:
```

Each actual node has its own `runtime` volume and unique `FSD_RENDERER_VERSION`.
The current Compute health endpoint is suitable for liveness, but its response
does not identify a node. Gateway identifies nodes from the configured/admin
`node_id`; a future `FSD_COMPUTE_NODE_ID` capability field is recommended but
not required for the MVP.

## 5. Data model

Gateway PostgreSQL is the source of truth for routing only. Platform PostgreSQL
remains the source of truth for user render jobs and assets. Compute SQLite is
node-local implementation state only.

### 5.1 ER diagram

```mermaid
erDiagram
  COMPUTE_NODES ||--o{ COMPUTE_RUNS : owns
  COMPUTE_NODES ||--o{ NODE_PROBES : receives
  COMPUTE_RUNS ||--o{ RUN_ARTIFACTS : exposes

  COMPUTE_NODES {
    uuid id PK
    varchar node_key UK
    varchar base_url UK
    varchar state
    int max_durable_slots
    int max_preview_slots
    jsonb capabilities_json
    timestamptz capabilities_at
    timestamptz last_healthy_at
    timestamptz created_at
    timestamptz updated_at
  }

  COMPUTE_RUNS {
    uuid gateway_run_id PK
    varchar idempotency_key UK
    char request_sha256
    uuid node_id FK
    varchar node_run_id UK
    varchar kind
    varchar state
    jsonb request_json
    jsonb last_status_json
    timestamptz reserved_at
    timestamptz terminal_at
    timestamptz created_at
    timestamptz updated_at
  }

  RUN_ARTIFACTS {
    uuid id PK
    uuid gateway_run_id FK
    varchar external_artifact_id UK
    varchar node_artifact_id
    varchar media_type
    bigint size_bytes
    char sha256
    timestamptz created_at
  }

  NODE_PROBES {
    bigint id PK
    uuid node_id FK
    timestamptz checked_at
    boolean healthy
    int latency_ms
    varchar error_code
  }
```

### 5.2 Tables and constraints

#### `compute_nodes`

| Column | Type | Rules |
|---|---|---|
| `id` | UUID | primary key |
| `node_key` | varchar(64) | unique, `[a-z0-9][a-z0-9-]{1,62}` |
| `base_url` | text | unique HTTPS or private HTTP URL; no path/query/userinfo |
| `state` | enum | `active`, `draining`, `offline`, `disabled` |
| `max_durable_slots` | int | `1..64`; default `1` for a GPU node |
| `max_preview_slots` | int | `1..64`; default `2` |
| `max_cpu_slots`, `max_gpu_slots` | int | independent durable CPU/GPU capacity; `1..64` |
| `max_cpu_preview_slots`, `max_gpu_preview_slots` | int | independent preview CPU/GPU capacity; `1..64` |
| `capabilities_json` | jsonb | latest validated `/capabilities` response |
| `capabilities_at`, `last_healthy_at` | timestamptz | nullable before first successful probe |

#### `compute_runs`

| Column | Type | Rules |
|---|---|---|
| `gateway_run_id` | UUID | primary key; returned as `computeRunId` |
| `idempotency_key` | varchar(200) | unique |
| `request_sha256` | char(64) | SHA-256 of canonical `{kind,payload}` JSON |
| `node_id` | UUID | FK to `compute_nodes`, assigned before upstream create |
| `node_run_id` | varchar(256) | null only while `allocating`; unique when present |
| `kind` | varchar(64) | requested persistent Compute kind |
| `state` | enum | `allocating`, `queued`, `running`, `completed`, `failed`, `cancelled`, `node_lost` |
| `request_json` | jsonb | original validated envelope; encrypted-at-rest if it can contain sensitive custom formulas |
| `last_status_json` | jsonb | latest normalized upstream status; never returned verbatim without filtering |
| `reserved_at`, `terminal_at` | timestamptz | supports capacity accounting/audit |

Unique constraint: `(node_id, node_run_id)` where `node_run_id IS NOT NULL`.

#### `run_artifacts`

Created only from a completed manifest. `external_artifact_id` is
`<gateway_run_id>:<safe-file-name>`. It maps to `node_artifact_id` such as
`<node_run_id>:<safe-file-name>`. The filename must pass the existing Platform
artifact ID safety rules: no `/`, `\\`, or `..`.

#### `node_probes`

Append-only operational history. Retain 7 days in MVP with a daily cleanup job.

### 5.3 Capacity query

The scheduler locks candidate `compute_nodes` rows in one database transaction.
For each active node it counts active `compute_runs` (in `allocating`, `queued`,
or `running`) **per resource pool**; this is its reserved durable capacity.
Select the lowest score:

```text
requested pools = engine cpu|openmp|avx2|avx512  -> {cpu}
                  engine cuda                    -> {gpu}
                  engine auto|hybrid             -> {cpu, gpu}

score = max(used[resource] / limit[resource] for resource in requested pools)
limit = max_cpu_slots for "cpu", max_gpu_slots for "gpu"
```

Tie-break by the oldest `last_assigned_at` (add this nullable timestamp to
`compute_nodes`) then `node_key`. A selected row gets a new `allocating` run
within that same transaction. This prevents two Gateway replicas overbooking a
single node. After locking a candidate the scheduler re-reads its fresh
per-resource counts, so concurrent reservations cannot oversubscribe one pool.

Notes on the pool semantics:

- An `auto`/`hybrid` request reserves **both** pools because actual execution
  may use or fall back across either processor; reserving both prevents either
  pool from being oversubscribed before execution is known. On a node with
  `max_cpu_slots=2, max_gpu_slots=1`, one `auto` run leaves one CPU slot free but
  no GPU slot, so a second `auto` run is rejected while an explicit `cpu` run can
  still be scheduled.
- Preview capacity works the same way with `max_cpu_preview_slots` /
  `max_gpu_preview_slots`; a preview reserves its requested pools in memory
  (`_preview_in_flight`) until the upstream call finishes.
- Legacy fields remain available: `usedDurableSlots`/`usedPreviewSlots` report
  the busiest pool (`max` of cpu/gpu) for backward compatibility, and
  `max_durable_slots`/`max_preview_slots` are the pre-split totals that the
  per-pool limits default to on upsert and migration.

### 5.4 Node configuration semantics

Capacity fields (`max_durable_slots`, `max_preview_slots`, and the CPU/GPU split)
are **declarative**: both `upsert_node` (operator admin API) and `bootstrap_node`
(runs after every Gateway restart) apply the template values verbatim. The
bootstrap template therefore wins over any ad-hoc operator tuning after a
restart — to change capacity durably, update the bootstrap template (the
`COMPUTE_GATEWAY_BOOTSTRAP_NODES_JSON` environment variable) or re-apply
`upsert_node` after each restart. `state` is runtime state and is **preserved**
across bootstrap for existing nodes.

## 6. Class diagram

```mermaid
classDiagram
  class ComputeV1Router {
    +capabilities() CapabilityResponse
    +preview(envelope) BinaryResponse
    +createRun(envelope) RunResponse
    +getRun(gatewayRunId) RunResponse
    +cancelRun(gatewayRunId) CancelResponse
    +manifest(gatewayRunId) ManifestResponse
    +streamArtifact(externalArtifactId) ByteStream
  }
  class GatewayAuth {
    +requireServiceKey(request)
  }
  class RunService {
    +reserveOrReplay(envelope) GatewayRun
    +submit(run) GatewayRun
    +refreshStatus(run) NormalizedStatus
    +cancel(run) CancelResponse
    +materializeManifest(run) ManifestResponse
  }
  class PreviewService {
    +render(envelope) BinaryResponse
  }
  class Scheduler {
    +selectDurableNode(kind,payload) ComputeNode
    +selectPreviewNode(kind,payload) ComputeNode
  }
  class NodeProbeService {
    +probeAll() void
    +refreshCapabilities(node) CapabilitySnapshot
  }
  class ComputeNodeClient {
    +createRun(node,envelope) NodeRun
    +preview(node,envelope) BinaryResponse
    +status(node,nodeRunId) NodeStatus
    +cancel(node,nodeRunId) NodeCancel
    +manifest(node,nodeRunId) NodeManifest
    +artifact(node,nodeArtifactId) ByteStream
  }
  class NodeRepository {
    +lockEligibleNodes() ComputeNode[]
    +saveProbe() void
  }
  class RunRepository {
    +reserveOrGet() GatewayRun
    +saveNodeRunId() void
    +markTerminal() void
    +findArtifact() RunArtifact
  }
  class ComputeNode
  class GatewayRun
  class RunArtifact

  ComputeV1Router --> GatewayAuth
  ComputeV1Router --> RunService
  ComputeV1Router --> PreviewService
  RunService --> Scheduler
  RunService --> RunRepository
  RunService --> ComputeNodeClient
  PreviewService --> Scheduler
  PreviewService --> ComputeNodeClient
  Scheduler --> NodeRepository
  NodeProbeService --> NodeRepository
  NodeProbeService --> ComputeNodeClient
  RunRepository --> GatewayRun
  NodeRepository --> ComputeNode
  RunRepository --> RunArtifact
```

## 7. Public private API contract

Gateway mirrors the current Compute v1 paths so Platform's existing
`ComputeClient` remains valid. JSON errors use the existing envelope:

```json
{"error":{"code":"MACHINE_CODE","message":"safe message","details":{}}}
```

### 7.1 Compute-compatible endpoints

| Method | Path | Gateway behavior |
|---|---|---|
| `GET` | `/compute/v1/health` | unauthenticated Gateway liveness; does not claim nodes are ready |
| `GET` | `/compute/v1/capabilities` | authenticated aggregate capability projection |
| `POST` | `/compute/v1/previews` | choose a healthy compatible node; proxy binary/JSON response |
| `POST` | `/compute/v1/runs` | reserve/replay route, forward to assigned node, return Gateway run ID |
| `GET` | `/compute/v1/runs/{gatewayRunId}` | resolve route and proxy normalized status |
| `POST` | `/compute/v1/runs/{gatewayRunId}/cancel` | resolve route and forward cancellation |
| `GET` | `/compute/v1/runs/{gatewayRunId}/manifest` | resolve route, validate/rewrite artifacts, persist mapping |
| `GET` | `/compute/v1/artifacts?artifactId=...` | resolve external artifact mapping and stream node bytes |

### 7.2 `POST /compute/v1/runs`

Request body is unchanged from Compute v1:

```json
{
  "schemaVersion": 1,
  "kind": "map_image",
  "idempotencyKey": "platform-job:8c3e73fe-3c8c-4e96-b412-8fbbb0954df6",
  "payload": {"width": 1024, "height": 1024}
}
```

Gateway validates basic envelope shape and `idempotencyKey` length, but leaves
kind-specific payload validation to the selected Compute node.

Success (`202`):

```json
{
  "schemaVersion": 1,
  "data": {
    "computeRunId": "8e17d79d-6706-4b33-9dda-ca122564cf4a",
    "status": "queued",
    "progress": {"percent": 0},
    "artifacts": []
  }
}
```

Gateway must not include `nodeId`, `nodeRunId`, URL, or scheduling score.

Create algorithm:

1. Canonicalize `{kind,payload}` and calculate SHA-256.
2. Lock/read by `idempotency_key`.
3. If present and hash differs, return `409 IDEMPOTENCY_CONFLICT`.
4. If present and hash matches, use its stored node and retry/read the same
   upstream creation; never select another node.
5. If absent, choose a compatible node under capacity lock and insert an
   `allocating` route with the original request JSON.
6. Forward the original body to that node with the original idempotency key.
7. Persist `node_run_id`, normalized initial status, and state. Return the
   Gateway UUID in place of the node run ID.

If the network call times out after node receipt, retain `allocating` and retry
the same node with the same idempotency key. Its local idempotency store makes
this safe. Do not release or reassign that reservation automatically.

### 7.3 Status, cancel, and manifest

Gateway validates `{gatewayRunId}` as UUID and loads the route.

- `GET /runs/{id}` forwards `GET /runs/{nodeRunId}`, normalizes the response,
  replaces only `computeRunId`, and updates `state`.
- `POST /cancel` forwards `{}` to node. It is idempotent. Gateway keeps the
  reservation until a subsequent status is terminal.
- `GET /manifest` requires completed upstream state. It validates every
  accepted artifact's media type, size, SHA-256, and safe filename; then writes
  `run_artifacts` idempotently and returns the rewritten artifact IDs.

Allowed artifact media types in MVP: `image/png`, `video/mp4`,
`model/gltf-binary`, and `application/sla`.

### 7.4 Artifact streaming

Request:

```text
GET /compute/v1/artifacts?artifactId=<gateway-run-uuid>:map.png
Authorization: Bearer <gateway key>
Range: bytes=0-1023    # optional
```

Gateway loads `run_artifacts`, constructs the corresponding node artifact ID,
and streams the node response without buffering it in memory. It forwards only
`Content-Type`, `Content-Length`, `Content-Range`, `Accept-Ranges`, and
`ETag` headers. It preserves `200`/`206` and maps upstream `404` to
`COMPUTE_ARTIFACT_NOT_FOUND`.

### 7.5 Capabilities projection

Gateway probes every active node. `/capabilities` returns the **union** of
capabilities available from healthy, schedulable nodes. The scheduler then
routes a request only to a node that supports its exact kind/features. This
keeps hardware-specialized nodes usable without advertising a feature for which
no eligible node remains. It also returns:

```json
{
  "schemaVersion": 1,
  "rendererVersion": "gateway",
  "gateway": {"healthyNodes": 2, "ready": true}
}
```

`ready` is true only when at least one node is healthy and has free durable or
preview capacity. Do not expose individual node addresses or hardware details
to Platform clients in MVP.

### 7.6 Operations API

This API has a separate `COMPUTE_GATEWAY_ADMIN_KEY`, is private-only, and is
not mounted through public Platform routing.

| Method | Path | Purpose |
|---|---|---|
| `PUT` | `/internal/v1/nodes/{nodeKey}` | create/update node URL, limits, and enabled state |
| `GET` | `/internal/v1/nodes` | operational node list with health/capacity counts |
| `POST` | `/internal/v1/nodes/{nodeKey}/drain` | set `draining`; no new assignments |
| `POST` | `/internal/v1/nodes/{nodeKey}/activate` | set `active` after successful probe |
| `POST` | `/internal/v1/nodes/{nodeKey}/disable` | disable new work immediately |

`PUT` request:

```json
{
  "baseUrl": "http://compute-node-a:18080",
  "maxDurableSlots": 1,
  "maxPreviewSlots": 2,
  "enabled": true
}
```

Gateway must probe health and authenticated capabilities before changing an
enabled node to `active`.

## 8. Scheduling policy

### 8.1 Durable jobs

Eligible nodes satisfy all conditions:

1. `state = active` and health age is at most `NODE_OFFLINE_AFTER_SECONDS`.
2. latest capabilities support requested persistent `kind`.
3. payload's requested engine/scalar/features are present in the node
   capability snapshot.
4. reserved durable count is lower than `max_durable_slots`.

Select by the lowest capacity score described in section 5.3. GPU nodes should
start with one durable slot because the existing C++ renderer already schedules
CPU/GPU tiles internally; allowing many simultaneous exports commonly causes
VRAM contention rather than higher throughput.

If no node is eligible, return `503 COMPUTE_CAPACITY_EXHAUSTED` with a retryable
message. Do not queue inside Gateway in MVP: Platform's durable outbox is the
single product-level retry mechanism.

### 8.2 Previews

Previews have no durable node-local route. The single MVP Gateway instance
selects a healthy compatible node with `in_flight_previews <
max_preview_slots`, increments an in-memory semaphore for that one request, and
releases it in `finally`. A preview timeout or connection error marks that node
suspect and may retry once on another eligible node only if no upstream response
headers were received. A future active-active Gateway needs DB/Redis preview
leases before adding replicas.

### 8.3 Reconciliation

Every probe interval, Gateway:

1. probes `/compute/v1/health` on active/draining nodes;
2. refreshes authorized `/capabilities` on successful nodes as needed;
3. marks nodes `offline` after the configured failure window;
4. polls non-terminal Gateway runs only when they have not been observed by
   Platform for two poll intervals;
5. marks a route `node_lost` if the assigned node stays offline while the run
   is non-terminal.

No active run is reassigned. Platform receives a retryable `503
COMPUTE_NODE_UNAVAILABLE` until the route becomes `node_lost`; then it receives
`502 COMPUTE_NODE_LOST` and can fail/recreate the Platform render job from its
immutable recipe snapshot.

## 9. Sequence diagrams

### 9.1 Durable create and idempotent retry

```mermaid
sequenceDiagram
  participant P as Platform Worker
  participant G as Compute Gateway
  participant D as Gateway DB
  participant N as Selected Compute Node

  P->>G: POST /runs (idempotencyKey, body)
  G->>D: lock key; select node; INSERT allocating route
  D-->>G: gatewayRunId + node A
  G->>N: POST /runs (same idempotencyKey, body)
  N-->>G: 202 nodeRunId, queued
  G->>D: save nodeRunId; state=queued
  G-->>P: 202 gatewayRunId, queued

  Note over P,N: Gateway crashes after upstream receive
  P->>G: POST /runs (same key, same body)
  G->>D: find allocating route assigned to A
  G->>N: POST /runs (same key, same body)
  N-->>G: replay nodeRunId, queued
  G->>D: persist nodeRunId
  G-->>P: 202 same gatewayRunId
```

### 9.2 Poll, manifest, and artifact ingestion

```mermaid
sequenceDiagram
  participant P as Platform Worker
  participant G as Compute Gateway
  participant D as Gateway DB
  participant N as Assigned Compute Node
  participant S as Object Storage

  P->>G: GET /runs/gatewayRunId
  G->>D: resolve route
  G->>N: GET /runs/nodeRunId
  N-->>G: completed + local artifacts
  G->>D: update state=completed
  G-->>P: completed + gatewayRunId

  P->>G: GET /runs/gatewayRunId/manifest
  G->>N: GET /runs/nodeRunId/manifest
  N-->>G: hashes, sizes, local artifact IDs
  G->>D: validate and persist artifact mapping
  G-->>P: manifest with external artifact IDs

  P->>G: GET /artifacts?artifactId=gatewayRunId:map.png
  G->>D: resolve artifact mapping
  G->>N: GET /artifacts?artifactId=nodeRunId:map.png
  N-->>G: stream bytes
  G-->>P: stream bytes
  P->>S: verify SHA-256 and upload
```

### 9.3 Node drain

```mermaid
sequenceDiagram
  participant O as Operator
  participant G as Compute Gateway
  participant D as Gateway DB
  participant N as Compute Node A
  participant P as Platform Worker

  O->>G: POST /internal/v1/nodes/A/drain
  G->>D: state=draining
  G-->>O: 200 active run count
  P->>G: POST /runs
  G->>D: exclude draining node A; select another node
  loop existing routes on A
    P->>G: GET /runs/gatewayRunId
    G->>N: GET /runs/nodeRunId
    N-->>G: terminal status
    G->>D: release reservation
  end
  O->>G: POST /internal/v1/nodes/A/disable
  G->>D: state=disabled
```

## 10. Error mapping

| Situation | HTTP | Gateway code | Retry policy |
|---|---:|---|---|
| invalid UUID/body/artifact ID | 422 | `COMPUTE_VALIDATION_ERROR` | no |
| missing/wrong Gateway key | 401 | `COMPUTE_UNAUTHORIZED` | no |
| unknown Gateway run/artifact | 404 | `COMPUTE_RUN_NOT_FOUND` / `COMPUTE_ARTIFACT_NOT_FOUND` | no |
| reused key with changed body | 409 | `IDEMPOTENCY_CONFLICT` | no |
| no compatible/free node | 503 | `COMPUTE_CAPACITY_EXHAUSTED` | yes |
| assigned node temporarily unavailable | 503 | `COMPUTE_NODE_UNAVAILABLE` | yes, same route/node |
| assigned node declared lost | 502 | `COMPUTE_NODE_LOST` | Platform must create a new job |
| node authentication failure | 503 | `COMPUTE_UPSTREAM_AUTH_FAILED` | no until config fixed |
| upstream request rejected | upstream 4xx | `COMPUTE_REJECTED` | no |
| upstream internal failure | 503 | `COMPUTE_UNAVAILABLE` | yes, same route/node |

Errors must contain no raw upstream body, URL, credentials, or local file path.

## 11. Implementation plan

### Milestone 1 — service foundation

- Create `compute-gateway/` Python service with FastAPI, `httpx`, SQLAlchemy,
  Alembic, and `asyncpg`.
- Add Dockerfile, `.env.example`, Compose services, private network wiring, and
  health endpoint.
- Create migrations for the four tables in section 5.
- Implement service/admin authentication and structured JSON error middleware.

### Milestone 2 — nodes and scheduling

- Implement node admin API and probe loop.
- Validate `/health` and `/capabilities` responses.
- Implement transactional eligible-node selection and durable reservations.
- Implement preview semaphore and capacity-aware routing.

### Milestone 3 — Compute v1 routing

- Implement immutable create/replay algorithm.
- Implement status/cancel/manifest routes and state transitions.
- Implement artifact ID rewriting plus streaming range proxy.
- Implement capabilities intersection.

### Milestone 4 — Platform integration

- Point API and outbox worker `COMPUTE_BASE_URL` at Gateway.
- Set Platform `COMPUTE_SERVICE_KEY` to Gateway key; set Gateway upstream key to
  C++ Compute key.
- Add integration contract suite with two real Compute containers/nodes.

### Milestone 5 — operations and release

- Add metrics, logs, readiness, drain runbook, and alerts.
- Run migration and rollback drill.
- Deploy one GPU node first, then add a second node and enable Gateway routing.

## 12. Acceptance criteria

The MVP is complete only when all of the following pass:

1. Two Compute nodes with separate runtime volumes register and become healthy.
2. Ten durable creates distribute according to configured capacity; every route
   records one owner node.
3. Repeating a create after a simulated Gateway crash returns the same Gateway
   run ID and creates no extra node run.
4. Status, cancellation, manifest, and artifact requests always reach the
   original node even while new work is assigned to another node.
5. Artifact streaming supports 200 and one byte range (206), and Platform's
   existing checksum ingestion succeeds through Gateway.
6. A drained node receives no new work and its existing runs remain readable.
7. An offline owner node yields explicit retryable/lost errors and never causes
   silent reassignment or duplicate durable execution.
8. Gateway service key, upstream Compute key, node URLs, node run IDs, and
   local artifact paths never appear in Platform/browser responses or logs.
9. Existing Platform Compute E2E passes with only `COMPUTE_BASE_URL` and
   `COMPUTE_SERVICE_KEY` changed.

## 13. Test matrix

| Layer | Required tests |
|---|---|
| Unit | canonical request hash; node eligibility; score/tie-break; state transition; artifact ID rewrite; auth/error mapping |
| Repository | concurrent same-key create; capacity lock; terminal release exactly once; `node_run_id` uniqueness |
| Contract | every mirrored Compute v1 route, JSON envelopes, binary previews, range stream, auth, 404/409/503 mapping |
| Integration | two fake nodes: sticky lifecycle, idempotent retry, drain, capacity exhaustion, offline node |
| Real E2E | two real C++ Compute containers with isolated runtime volumes; Platform image/video/mesh render and verified S3 ingestion |
| Resilience | kill Gateway during create; kill owner node during run; restart Gateway; database retry; upstream timeout |

## 14. Observability and operations

Required structured log fields: `request_id`, `gateway_run_id`, `node_key`,
`node_run_id` (internal logs only), `idempotency_key` (hashed in production),
`kind`, `operation`, `latency_ms`, and `outcome`.

Required metrics:

- `compute_gateway_node_health{node_key}`
- `compute_gateway_node_durable_slots_used{node_key}`
- `compute_gateway_preview_in_flight{node_key}`
- `compute_gateway_runs_total{kind,state}`
- `compute_gateway_upstream_requests_total{operation,status}`
- `compute_gateway_upstream_latency_seconds{operation}`
- `compute_gateway_node_lost_total{node_key}`

Alert when no active healthy node exists, a node stays offline for five minutes,
capacity is exhausted for five minutes, or any `node_lost` occurs.

## 15. Open implementation decisions deliberately fixed for MVP

| Decision | MVP choice | Reason |
|---|---|---|
| Gateway state store | dedicated PostgreSQL | durable routing and transactional scheduling |
| Durable job assignment | sticky database route | Compute state/artifacts are node-local |
| Load algorithm | compatible least-reserved-capacity | predictable, simple, safe for GPU nodes |
| GPU durable concurrency | one slot/node by default | avoids VRAM oversubscription with current renderer |
| Preview routing | stateless least-loaded | no run affinity needed |
| Gateway run identifier | UUID | opaque, stable, compatible with Platform client |
| Active run migration | unsupported | cannot transfer local runtime/cancellation state safely |
| Artifact handling | streaming proxy with rewritten IDs | preserves current Platform checksum ingestion and hides nodes |
| Node configuration | private admin API plus PostgreSQL | supports remote machines without a new node agent |

This specification intentionally keeps the C++ Compute contract intact. Gateway
is the place where distributed ownership is introduced; Compute nodes remain
single-host render executors.
