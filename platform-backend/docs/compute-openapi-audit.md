# Compute OpenAPI Audit — Closed

Original audit from 2026-07-23 examined legacy `/api/*` routes and is superseded.

Current C++ private contract is Compute v1:

```text
POST /compute/v1/previews
POST /compute/v1/runs
GET  /compute/v1/runs/{computeRunId}
POST /compute/v1/runs/{computeRunId}/cancel
GET  /compute/v1/runs/{computeRunId}/manifest
GET  /compute/v1/artifacts?artifactId=...
```

It requires bearer auth outside health and durable requests use envelope
`schemaVersion`, `kind`, `payload`, `idempotencyKey`. Platform implementation
and its local contract stub use this contract; see
[`compute-openapi.yaml`](compute-openapi.yaml),
[`platform-backend-spec.md`](platform-backend-spec.md), and
`backend/src/{core/http_server.cpp,api/routes_compute_v1.cpp}`.

Legacy `/api/*` remains a C++ local compatibility surface and is not a Platform
Worker dependency.
