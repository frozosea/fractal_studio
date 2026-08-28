# Architecture / 架构

本文描述当前商业产品的代码分层、生产数据流和扩展边界。早期 Vue/Vite 直连 C++ 的原型已不
再是浏览器依赖；当前浏览器只访问 Next.js 和 FastAPI Platform，所有 Compute 请求经有状态
Gateway 路由到 0..N 个私有 C++ 节点。

生产安装与运维见 [ops/production/INSTALL.md](../ops/production/INSTALL.md)，实际线上快照见
[ops/production/STATUS.md](../ops/production/STATUS.md)。

## 1. Runtime topology / 运行拓扑

```text
Browser
  |
  v
Caddy (canonical HTTPS origin)
  +-- / and localized pages ------> Next.js Frontend
  +-- /platform/* ----------------> FastAPI Platform API
  +-- /fractal-platform/* --------> MinIO signed object path

Platform API / workers
  +-- Platform PostgreSQL   users, sessions, recipes, jobs, commerce, assets
  +-- Redis                 preview queue, cache and short-lived coordination
  +-- MinIO                 authoritative commercial objects
  +-- external providers    Alipay and optional Studio AI provider
  |
  v
Compute Gateway + Gateway PostgreSQL
  |
  | health/capability filtering, CPU/GPU capacity, durable-run affinity
  v
0..N C++ Compute nodes over WireGuard
```

Local Docker development keeps the same service boundaries but runs two local Compute containers to
exercise distribution and affinity. The local two-node fixture is a test topology, not a production
limit.

## 2. Service ownership

| Service | Source | Owns | Must not own |
|---|---|---|---|
| Frontend | `frontend/` | Next.js routes, browser interaction, localized UI state | provider keys, direct Compute credentials |
| Platform | `platform-backend/` | identity, CSRF, recipes, quota, jobs, assets, marketplace, orders, membership, payouts, AI audit/history | node scheduling, Compute runtime files |
| Gateway | `compute-gateway/` | node registry, probes, capabilities, resource slots, run-to-node affinity, external artifact routing | users, money, quota, commercial objects |
| Compute | `backend/` | validated fractal math, CPU/CUDA execution, local run lifecycle, manifest and temporary artifacts | accounts, orders, entitlements, authoritative assets |
| Platform PostgreSQL | VPS | commercial state and transactional outbox | node-local execution state |
| Gateway PostgreSQL | VPS | node state, probes and durable route identity | commercial state |
| Redis | VPS | bounded queue/cache/coordination state | durable source of truth |
| MinIO | VPS | encrypted commercial artifacts and previews | raw Compute scheduling state |

This ownership split is also the failure boundary: zero healthy Compute nodes must not prevent the
control plane from starting or serving identity, commerce, marketplace, assets and existing
downloads.

## 3. Main request flows

### 3.1 Interactive Studio preview

1. Browser validates and normalizes Studio state, then requests a Platform preview job.
2. Platform authenticates the user, applies rate/resource limits and writes bounded queue state.
3. Preview worker maps the canonical recipe to a Compute v1 envelope.
4. Gateway filters all active healthy nodes by capabilities and requested CPU/GPU pools, reserves an
   in-memory preview slot and forwards the request.
5. Compute validates the envelope and returns preview bytes plus execution metadata.
6. Platform verifies/encodes the response and returns it to the browser; superseded browser requests
   cannot overwrite the latest view.

Preview capacity exhaustion is explicit. Platform must not create durable jobs or consume export
quota as a substitute for a failed preview.

### 3.2 Durable render and artifact ingestion

1. Browser creates or reuses an immutable recipe and submits a render request with idempotency.
2. Platform atomically checks/reserves quota, persists a render job and schedules work through its
   transactional outbox.
3. Worker submits a Compute v1 durable run to Gateway.
4. Gateway selects one compatible healthy node, records the node affinity with the idempotency hash,
   then forwards creation. Replays return the same Gateway run.
5. Poll, cancel, manifest and artifact requests always return to that original node.
6. After completion, Platform verifies manifest, media limits, size and SHA-256, uploads the object to
   encrypted VPS MinIO and records the commercial asset.
7. Browser receives an authorized, short-lived signed download URL. Internal MinIO or Compute
   addresses are never exposed.

Compute runtime remains necessary while a run is being polled or ingested, but it is not the final
asset store. A node cannot be deleted merely because the commercial object eventually lives in
MinIO.

### 3.3 Marketplace and commerce

Marketplace reads published listings and public derivatives from Platform state. Checkout,
entitlement and membership changes are transactional and idempotent. Alipay callbacks terminate at
the canonical VPS origin, are verified server-side and schedule follow-up work through Platform;
they never depend on a Compute node being online.

Private master assets require an entitlement check before Platform issues a signed URL. Caddy
preserves the S3-signed `/fractal-platform/*` path when proxying to MinIO.

### 3.4 Studio AI

Browser requests go to Platform and stream back over the `/platform` proxy. Only the Platform API
process receives the provider key. Images remain bounded and in memory; prompts, private recipes and
provider credentials are excluded from ordinary logs.

Provider output is untrusted. Formula/sequence/location/color patches pass schema, range, resource
budget and trusted-context validation before the UI may apply them; undo remains a client-side
operation. Deterministic tests use mocks, while paid provider contracts are explicit manual checks.

Disabling `AI_ENABLED` removes only AI functionality. Studio rendering and all non-AI control-plane
features continue normally.

## 4. Frontend layers

The commercial browser application is Next.js 14, React 18 and `next-intl`:

| Layer | Files | Responsibility |
|---|---|---|
| Routes/layouts | `frontend/src/app/[locale]/` | public, auth and workbench route groups; localized layouts and errors |
| Studio | `components/studio/`, `stores/studio-store.ts` | canvas interaction, canonical recipe edits, preview state and AI assistant |
| Product UI | `components/layout/`, `components/listings/`, `components/shared/` | shell, marketplace cards, listing AI and reusable states |
| API boundary | `lib/api/platform.ts`, `lib/api/errors.ts` | same-origin Platform requests, DTO conversion and stable errors |
| Validation | `lib/validators/`, `lib/studio-catalog.ts` | browser-side schema/range checks and supported product catalog |
| Providers/state | `providers/`, `stores/` | auth, React Query, theme and local UI state |
| Localization | `src/i18n/`, `messages/*.json` | locale routing and user-facing copy |

Frontend code may improve responsiveness and reject invalid input early, but Platform remains the
authorization and quota boundary. No `NEXT_PUBLIC_*` variable may contain a service or provider key.

## 5. Platform layers

`platform-backend/` is a FastAPI modular monolith. Each product module generally follows
router -> service -> repository/port boundaries:

- `auth/`: users, sessions, CSRF, creator profiles and RBAC;
- `studio/`: recipes, preview queue, render jobs, quota and Compute mapping;
- `ai/`: conversations, grounded exploration, listing copy, provider adapter and cleanup;
- `assets/`: asset reads, derivatives, signed downloads and cleanup;
- `marketplace/`: listings, facets, favorites and moderation-facing data;
- `commerce/`, `membership/`, `finance/`: orders, entitlements, membership and payouts;
- `admin/`: privileged account, marketplace, statistics and Compute monitoring views;
- `outbox/`: at-least-once background dispatch and retries;
- `infrastructure/`: Compute, Redis, storage and payment adapters;
- `core/`: settings, database, idempotency, audit and request/logging boundaries.

Schema migrations live in `platform-backend/migrations/`. API, worker and preview-worker are separate
processes built from the same release image and must be updated together.

## 6. Gateway scheduling and affinity

Gateway accepts a declarative JSON array of any number of nodes. Each node has a stable `nodeKey`,
private base URL, enabled intent and separate CPU/GPU durable/preview capacities.

For a request, Gateway:

1. loads all active nodes;
2. removes stale/unhealthy and capability-incompatible nodes;
3. determines required CPU/GPU resource pools (`auto`/`hybrid` reserve both);
4. compares current reservations with per-pool limits;
5. chooses the least-loaded candidate, breaking ties by oldest assignment and node key;
6. locks only the selected database row and rechecks capacity before reserving;
7. persists durable run affinity before forwarding upstream.

Health probes automatically move offline nodes back into scheduling. Human draining/disabled state is
preserved across bootstrap; declarative capacity is reapplied on every Gateway restart. Missing all
eligible nodes produces `503 COMPUTE_CAPACITY_EXHAUSTED`, not an unbounded queue.

Gateway implementation and schema details are in
[compute-gateway-mvp/README.md](compute-gateway-mvp/README.md).

## 7. Compute layers and runtime

| Layer | Files | Responsibility |
|---|---|---|
| Entry | `backend/src/main.cpp` | locate runtime, open SQLite, construct `JobRunner`, start HTTP service |
| HTTP core | `backend/src/core/http_server.cpp` | private authentication, parsing, route dispatch and streaming |
| Runtime core | `backend/src/core/` | run state, cancellation, path safety, resources and hardware evidence |
| API routes | `backend/src/api/` | Compute v1 DTO validation, pipeline calls, manifests and artifacts |
| Kernels | `backend/src/compute/` | 2D, transition, video, 3D, special points, scalar/SIMD/CUDA paths |
| Hardware adapters | `backend/src/adapters/` | OpenMP/CUDA probing and execution capability evidence |
| Contract tests | `backend/src/tests/compute_v1/` | real-process HTTP behavior, artifacts, validation and hardware contracts |

Compute stores node-local metadata in `runtime/db/` and run files in `runtime/runs/`. Artifact IDs are
run-relative and canonical-containment checked; content streams support bounded range requests.
Custom formulas use a typed parser and bounded bytecode, not arbitrary native compilation.

The current product exposes 2D map, Julia, pair/multi transition, safe Formula/Sequence and PNG
export. Compute also retains contracted video, 3D, field and special-point jobs for later product
phases; capability advertisement, not a UI assumption, decides whether a node may receive a job.

## 8. Trust and failure boundaries

- Browser -> Platform: session/bearer auth, CSRF for cookie writes, CORS/canonical-origin checks.
- Platform -> Gateway: service key; only the API process may receive the separate admin key for node
  monitoring.
- Gateway -> Compute: a private upstream service key over WireGuard.
- Platform -> MinIO: internal credentials and mandatory server-side encryption.
- Platform -> Alipay/AI: provider credentials exist only in server-side processes that need them.
- Logs: whitelist operational fields; exclude passwords, keys, prompts, private formulas and images.

Long operations need stable idempotency, timeout, cancellation, bounded retry and observable terminal
state. Platform quota is reserved only when work is accepted and follows explicit refund/release rules
on failure or cancellation.

## 9. Where to add a feature

1. Define the user and API contract, error semantics, quota behavior and compatibility first.
2. Add Platform request/response models and service rules; keep authorization out of adapters.
3. Extend Compute v1 only when new math or artifact work is required. Update capabilities and real
   contract tests in the same change.
4. Gateway changes are needed only for scheduling, transport or affinity semantics; never branch on a
   fixed node count or hardware name.
5. Add Frontend API DTOs/validation before page components; preserve localized and mobile behavior.
6. Cover zero-node/provider-offline paths, idempotent replay, cancellation, private data and resource
   limits.
7. Update production templates/runbooks separately from product behavior and roll out behind a feature
   flag where appropriate.

Pipeline-specific references remain in `docs/render_pipeline.md`, `docs/coloring_contract.md`,
`docs/special_points.md`, `docs/3d_pipeline.md`, `docs/video_pipeline.md`,
`docs/compute_v1_contract.md`, `docs/compute_v1_jobs.md` and
`docs/platform_compute_integration.md`.
