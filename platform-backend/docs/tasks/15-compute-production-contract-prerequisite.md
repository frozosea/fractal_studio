# T15 — Compute Production Contract Prerequisite

## Task description

Implement the C++ Compute production contract required before Platform switches from its E2E stub to a real private Compute service. This is an external prerequisite owned by the Compute codebase, tracked here because M2/M3 production acceptance depends on it.

## Work scope

- ../fractal_studio-master/backend/src/core/http_server.cpp and the C++ route/auth/service layers it delegates to
- platform/backend/docs/compute-openapi.yaml, platform/backend/docs/compute-spec.md
- Platform Compute contract fixtures in tests/e2e/fixtures/compute_stub
- tests/e2e/test_render_jobs.py and tests/e2e/test_asset_ingestion.py

## Goal

Provide an authenticated, idempotent, bounded, and unambiguous private Compute API that Platform can safely retry and ingest from.

## Acceptance criteria

- Every private route requires Authorization: Bearer service key; missing/unknown/revoked key is 401 and a key without render scope is 403. Key IDs support two active hashes for rotation.
- Durable submission accepts unique clientJobId and returns a stable run result under replay; request limits and standard error body/status enum match compute-openapi.yaml.
- POST /api/runs/cancel is the only production cancellation route. The legacy POST /api/runs/{runId}/cancel route is removed or rejects use with a migration-safe documented response.
- Run status/artifact enumeration expose stable artifact IDs, kinds, sizes, and checksums/manifest data required by Platform. Artifact content remains private and has no browser credential path.
- Inline preview preserves RGBA8 plus X-FSD-width/X-FSD-height contract. Still/video/mesh routes enforce declared input/output limits.

## Specification source

Verified Current C++ Compute Contract And Required Production Contract; Compute auth contract; M2 responsibilities; M3 ComputeArtifactReader boundary rules; MVP Technical Limits; Transaction And Consistency Policy; Sequence Diagrams 1 and 5.

## Dependencies

- T00 canonical Compute delivery record; Compute repository owner and deployment environment.
- C++ HTTP/auth implementation, cryptographic key hashing, compute-openapi.yaml, and contract-test tooling.
- T06 and T07 remain stub-compatible but cannot pass real-Compute production acceptance until this task is complete.
- No browser receives Compute key or reaches Compute routes directly.

## Test plan

~~~bash
docker compose -f docker-compose.dev.yml --profile e2e up --build -d
curl --noproxy '*' -sS -f -b /tmp/studio.cookie -H 'Idempotency-Key: real-compute-job-0001' -H 'Content-Type: application/json' -d '{"recipeId":"RECIPE_ID","output":{"kind":"image","format":"png","width":512,"height":512}}' http://localhost:8000/v1/render-jobs
curl --noproxy '*' -sS -f -b /tmp/studio.cookie http://localhost:8000/v1/render-jobs/RENDER_JOB_ID
curl --noproxy '*' -sS -f -b /tmp/studio.cookie -H 'Idempotency-Key: real-compute-cancel-0001' -X POST http://localhost:8000/v1/render-jobs/CANCELLABLE_RENDER_JOB_ID/cancel
~~~

Run the same contract suite directly against private Compute with missing, wrong-scope, old, and rotated keys; then run the Platform API flow above against real Compute. Assert one clientJobId submission under worker retry, no Platform use of the legacy cancel route, verified checksum/manifest ingestion, and no Compute response field leaking through Platform API.

## Implementation plan

1. Align C++ routes and DTOs with compute-openapi.yaml; add standard error serializer and request validation/limits.
2. Add service-key verification, scopes, key IDs, dual-hash rotation, and audit-safe logging.
3. Persist/enforce clientJobId idempotency and stable run/artifact metadata with checksums/manifest.
4. Consolidate cancellation to POST /api/runs/cancel and remove/disable the legacy path.
5. Add contract tests covering auth, replay, limits, status, artifact integrity, and cancellation.
6. Replace only the E2E Compute stub profile after Platform T06/T07 real-service E2E passes.
