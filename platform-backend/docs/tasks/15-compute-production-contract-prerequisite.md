# T15 — Compute v1 Deployment Acceptance

## Task description

Verify deployment of the implemented C++ Compute v1 contract before Platform switches from its E2E stub to a real private Compute service.

## Work scope

- ../fractal_studio-master/backend/src/core/http_server.cpp and the C++ route/auth/service layers it delegates to
- platform/backend/docs/compute-openapi.yaml, platform/backend/docs/compute-spec.md
- Platform Compute contract fixtures in tests/e2e/fixtures/compute_stub
- tests/e2e/test_render_jobs.py and tests/e2e/test_asset_ingestion.py

## Goal

Deploy and prove the authenticated, idempotent, bounded private Compute v1 API that Platform retries and ingests from.

## Acceptance criteria

- Every private Compute v1 route requires `Authorization: Bearer` service key; missing/unknown key is 401.
- Durable `POST /compute/v1/runs` accepts stable `idempotencyKey` and returns a stable run result under replay; request limits and error envelope match compute-openapi.yaml.
- `POST /compute/v1/runs/{id}/cancel` is the only Platform cancellation route; legacy `/api/*` is disabled in production.
- Run status/artifact enumeration expose stable artifact IDs, kinds, sizes, and checksums/manifest data required by Platform. Artifact content remains private and has no browser credential path.
- Inline preview preserves RGBA8 plus X-FSD-width/X-FSD-height contract. Still/video/mesh routes enforce declared input/output limits.

## Specification source

Verified Current C++ Compute Contract And Required Production Contract; Compute auth contract; M2 responsibilities; M3 ComputeArtifactReader boundary rules; MVP Technical Limits; Transaction And Consistency Policy; Sequence Diagrams 1 and 5.

## Dependencies

- T00 canonical Compute delivery record; Compute repository owner and deployment environment.
- C++ HTTP/auth implementation, cryptographic key hashing, compute-openapi.yaml, and contract-test tooling.
- T06 and T07 are stub-compatible; this task provides production deployment evidence against a real Compute v1 process.
- No browser receives Compute key or reaches Compute routes directly.

## Test plan

~~~bash
docker compose -f docker-compose.dev.yml --profile e2e up --build -d
curl --noproxy '*' -sS -f -b /tmp/studio.cookie -H 'Idempotency-Key: real-compute-job-0001' -H 'Content-Type: application/json' -d '{"recipeId":"RECIPE_ID","output":{"kind":"image","format":"png","width":512,"height":512}}' http://localhost:8000/v1/render-jobs
curl --noproxy '*' -sS -f -b /tmp/studio.cookie http://localhost:8000/v1/render-jobs/RENDER_JOB_ID
curl --noproxy '*' -sS -f -b /tmp/studio.cookie -H 'Idempotency-Key: real-compute-cancel-0001' -X POST http://localhost:8000/v1/render-jobs/CANCELLABLE_RENDER_JOB_ID/cancel
~~~

Run the contract suite directly against private Compute with missing and valid keys; then run Platform API flow against real Compute. Assert one replay key submission under worker retry, no Platform use of legacy routes, verified checksum/manifest ingestion, and no Compute response field leaking through Platform API.

## Implementation plan

1. Deploy Compute with `FSD_COMPUTE_SERVICE_KEY` and `FSD_ENABLE_LEGACY_API=0`.
2. Run C++ Compute v1 contract tests and Platform real-service E2E.
3. Record key-rotation, private-network and artifact-retention operational evidence.
