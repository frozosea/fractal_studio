# T14 — Full MVP End-to-End Regression Suite

## Task description

Create the release-level black-box suite that proves the complete MVP through the deployed local development environment: authentication, preview, durable render, asset ingest/derivatives, listing, checkout, authoritative payment, entitlement download, cancellation, reconciliation, and manual payout. This task does not replace module-level E2E tests; it joins them into stable user journeys.

## Work scope

- tests/conftest.py, tests/e2e/{test_mvp_happy_path.py,test_failure_recovery.py,test_security_boundaries.py,test_api_contract_matrix.py}
- tests/e2e/fixtures/{compute_stub,alipay_stub,qr.png}
- docker-compose.dev.yml, .env.example, scripts/e2e.sh
- app/main.py and test-double configuration only where required for black-box setup

## Goal

Produce repeatable evidence that the full Compose development stack implements the specified browser API and failure/retry behavior without direct database mutation or host service dependencies.

## Acceptance criteria

- One command resets isolated test state, builds/starts full dev stack, bootstraps only fixed E2E finance_operator/disabled fixtures, waits for readiness, runs all HTTP E2E cases, and tears down temporary resources.
- Happy path proves creator registration/profile -> recipe -> render -> asset/derivatives -> listing publish -> buyer checkout -> signed Alipay success -> entitlement -> protected master download -> creator payout request -> operator paid settlement.
- Failure path proves duplicate idempotency, invalid authorization, preview rate limit, job cancellation, Compute retry/lease recovery, invalid and duplicate webhook, delayed reconciliation, reversal/manual review, and payout race/cleanup.
- Assertions inspect API status/body/headers and externally observable stub requests only; business state is never set with direct test SQL. Profile-scoped startup fixture seeding is permitted only for the otherwise-unreachable finance_operator and disabled-user identities defined by T00.
- Fixture credentials and fake keys are scoped to test Compose only and are never production defaults.
- Every public route has a positive or authorized-read case, every browser mutation has replay/conflict coverage, and every baseline error code has an exact envelope/code assertion.

## Specification source

Public API; Transport Rules; MVP Technical Limits; M1–M7 module sections; Transaction And Consistency Policy; Logging And Request Correlation Policy; State Machines And Database Checks; all six Sequence Diagrams.

## Dependencies

- T00 through T13.
- Docker Compose, pytest/httpx or shell runner, contract-compatible Compute and Alipay test doubles, MinIO, PostgreSQL, Redis.
- Tests depend on documented API contracts and safe testing-only configuration. They must not add undocumented production endpoints.
- Worker timing uses polling/deadlines rather than arbitrary long sleeps; test double provides deterministic queued/running/completed/error/query states.

## Test plan

~~~bash
docker compose -f docker-compose.dev.yml --profile e2e up --build -d
./scripts/e2e.sh
curl --noproxy '*' -sS -f http://localhost:8000/healthz
curl --noproxy '*' -sS -f http://localhost:8000/v1/explore?sort=newest&limit=24
docker compose -f docker-compose.dev.yml --profile e2e logs api worker compute-stub alipay-stub --tail=200
~~~

scripts/e2e.sh must execute explicit requests equivalent to:

1. POST /v1/auth/register and PATCH /v1/me/creator-profile with cookie jar and Idempotency-Key.
2. POST /v1/recipes, POST /v1/render-jobs, poll GET /v1/render-jobs/{id}, then GET /v1/me/assets/{id}.
3. POST /v1/listings, POST /v1/listings/{id}/publish, GET /v1/explore.
4. POST /v1/checkout, POST /v1/webhooks/alipay with a stub-signed form, GET /v1/orders/{id}, POST /v1/assets/{id}/download-url.
5. POST /v1/me/payout-requests multipart, GET /internal/v1/payout-requests, POST /internal/v1/payout-requests/{id}/mark-paid.
6. Repeat these requests under invalid session/role/key/signature, duplicate event, Compute timeout, worker restart, and reversal states.

The API contract matrix executes the following additional concrete HTTP requests; IDs are captured from prior JSON data envelopes:

| API calls | Required assertion |
|---|---|
| POST /v1/auth/login; POST /v1/auth/logout; GET /v1/me | Login rotates cookie; logout revokes it; unauthenticated GET returns the exact 401 envelope. |
| POST /v1/studio/preview; POST /v1/recipes; GET /v1/me/recipes | Preview is PNG/non-persistent; recipe create/reuse is 201/200; list is a cursor collection envelope. |
| POST /v1/render-jobs; GET /v1/render-jobs/{renderJobId}; POST /v1/render-jobs/{renderJobId}/cancel | Creation is 202/outbox-backed; owner-only polling works; cancellable state reaches cancelled; compute_succeeded cancellation is 409 invalid_state. |
| GET /v1/me/assets; GET /v1/me/assets/{assetId}; PATCH /v1/me/assets/{assetId}; DELETE /v1/me/assets/{assetId}; POST /v1/assets/{assetId}/download-url | Views omit internal object/Compute fields; hide/restore and soft-delete obey listing/sale retention; owner/entitled URL works and unrelated user is 403. |
| GET /v1/explore; GET /v1/listings/{listingId}; GET /v1/me/listings; POST /v1/listings; PATCH /v1/listings/{listingId}; POST /v1/listings/{listingId}/publish; POST /v1/listings/{listingId}/unpublish | Published-only feed/cursor behavior, owner draft visibility, draft-only edit, immutable version on publish, and state transition are asserted. |
| GET /v1/me/favorites; POST /v1/assets/{assetId}/favorite; DELETE /v1/assets/{assetId}/favorite | Bookmark is idempotent per user/asset and never changes catalogue ranking. |
| POST /v1/checkout; GET /v1/orders/{orderId}; GET /v1/me/purchases; POST /v1/webhooks/alipay | Checkout has frozen Decimal snapshot; only buyer reads orders; signed webhook is authoritative and exact success is returned after commit. |
| POST /v1/me/payout-requests; GET /v1/me/payout-requests; POST /v1/me/payout-requests/{payoutRequestId}/cancel | Creator receives no QR key/operator reference; pending cancel releases reserve; replay/conflict uses multipart body hash including QR SHA-256. |
| GET /internal/v1/payout-requests; POST /internal/v1/payout-requests/{payoutRequestId}/mark-paid; POST /internal/v1/payout-requests/{payoutRequestId}/reject | Only E2E-seeded finance_operator succeeds; QR URL has ten-minute expiry; paid/rejected journal/audit state is correct. |

For each mutation above, execute the same Idempotency-Key and body twice, then reuse the key with a different body. Assert replayed status/body/headers and 409 idempotency_conflict respectively. For error cases assert the exact common envelope and one of 401 unauthenticated, 403 forbidden, 404 not_found, 409 invalid_state, 413 payload_too_large, 422 validation_error, 429 quota_exceeded, 502 compute_error, or 503 payment_unavailable.

## Implementation plan

1. Build deterministic Compute and Alipay stubs from compute-openapi.yaml and payment gateway contract.
2. Add Compose profile/environment for isolated E2E services, buckets, keys, and data reset.
3. Add reusable HTTP client helpers for cookie jars, UUID extraction, asynchronous polling, form signing, and fixed E2E fixture login.
4. Implement the API contract matrix below in addition to happy-path, failure/recovery, and information-disclosure tests.
5. Assert data/collection/error envelopes, decimal strings, cursor mismatch 422, request IDs, and all baseline error codes.
6. Capture request IDs and redacted logs on failure; make reports actionable.
7. Gate release on full suite after module task suites pass.
