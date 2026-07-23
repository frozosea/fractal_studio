# T06 — M2 Durable Render Jobs and Compute Worker

## Task description

Implement image, video, HS-mesh, and transition-mesh render jobs. The API atomically creates a job, PostgreSQL quota reservation, and outbox event. The worker submits/polls/cancels Compute outside DB locks, preserves mapper version, and hands successful selected artifacts to M3.

## Work scope

- app/studio/{router.py,models.py,render_job_service.py,render_job_repository.py,render_worker.py,compute_request_mapper.py,quota_service.py}
- app/infrastructure/compute/compute_client.py
- app/outbox/{service.py,worker.py}
- tests/e2e/test_render_jobs.py

## Goal

Provide idempotent durable Compute work with correct state transitions, cancellation, quota accounting, and no browser access to Compute.

## Acceptance criteria

- POST /v1/render-jobs atomically stores queued job, immutable output spec/mapping_version, PostgreSQL reservation, and render.created.v1; it returns 202 before Compute submission.
- Allowed outputs only: image/png, video/mp4, HS mesh glb|stl, transition mesh glb|stl; all limits are validated.
- Worker submits clientJobId=render_job.id once, stores runId, reuses the same poll row, updates progress, and never remaps a saved job.
- Cancellation uses only POST /api/runs/cancel. It is CAS/row-locked; cancellation after compute_succeeded is 409; terminal transitions release quota once.
- Production integration is blocked until Compute implements compute-openapi.yaml: Bearer auth, clientJobId uniqueness, standard errors, limits, checksums/manifest contract, and one cancel route.

## Specification source

M2. Studio And Render Job Module; Responsibilities And Dependencies; Verified Current C++ Compute Contract And Required Production Contract; Transaction And Consistency Policy (create/cancel render); Event routing; State Machines And Database Checks; Sequence Diagrams 1 and 5; Public API M2.

## Dependencies

- T02, T03, T04, T05.
- SQLAlchemy async transactions/row locks, httpx, Pydantic; Compute service key and contract-compatible Compute stub.
- M3 AssetIngestionPort is a narrow port implemented by T07. No direct M3 repository/S3 call from RenderJobService.
- PostgreSQL quota is authoritative; Redis is preview-only.

## Test plan

~~~bash
docker compose -f docker-compose.dev.yml up --build -d
curl --noproxy '*' -sS -f -b /tmp/studio.cookie -H 'Idempotency-Key: job-0001' -H 'Content-Type: application/json' -d '{"recipeId":"RECIPE_ID","output":{"kind":"image","format":"png","width":512,"height":512}}' http://localhost:8000/v1/render-jobs
curl --noproxy '*' -sS -f -b /tmp/studio.cookie http://localhost:8000/v1/render-jobs/RENDER_JOB_ID
curl --noproxy '*' -sS -f -b /tmp/studio.cookie -H 'Idempotency-Key: cancel-0001' -X POST http://localhost:8000/v1/render-jobs/RENDER_JOB_ID/cancel
~~~

Use Compute stub modes queued, running, completed, and transient failure. Assert 202 queued, then running/completed via GET, one submission for clientJobId, one quota release, and a ready asset only after T07 ingest. Create a second job and cancel before submission; assert 202 then cancelled. Force compute_succeeded and assert cancel returns 409.

## Implementation plan

1. Add strict output union and technical limit validation.
2. Create job/reservation/outbox in one short transaction with idempotency and audit.
3. Implement submit, poll, cancellation, and terminal CAS transitions in RenderWorker.
4. Persist Compute result metadata and selected allowlisted artifact IDs only.
5. Reschedule the same poll event; never create an unbounded poll-event stream.
6. Inject M3 ingestion port; map Compute failures to stable error codes.
