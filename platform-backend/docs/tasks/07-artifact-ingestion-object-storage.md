# T07 — M3 Render-Artifact Ingestion and Object Storage

## Task description

Implement the M3 ingestion port called by the render worker. It creates a processing asset, downloads only selected Compute artifacts, validates bytes and SHA-256, uploads a private master and provenance manifest, then atomically marks the asset ready, completes the job, releases quota, and queues derivatives.

## Work scope

- app/assets/{models.py,ports.py,repository.py,service.py}
- app/infrastructure/{compute/compute_artifact_reader.py,storage/object_storage.py}
- app/studio/render_worker.py, app/outbox/service.py
- tests/e2e/test_asset_ingestion.py

## Goal

Move a verified private master from Compute to storage with an explicit recovery path for every partial failure; never expose Compute paths, run IDs, artifact IDs, or S3 keys to browser clients.

## Acceptance criteria

- Exactly one ingestion path first commits Asset(processing, private), then performs external I/O outside DB locks, then commits master AssetFile, Asset(ready), completed job, quota release, and media.create_derivatives.v1.
- Reader rejects malformed runId:fileName, path traversal, path separators, unselected artifacts, non-allowlisted extension, and artifacts over 500 MiB; Platform calculates SHA-256 itself.
- Master object key follows private/masters/{asset_id}/{asset_file_id}/{original_name}; generated private render_manifest records route, run ID, mapper version, selected IDs, and calculated checksums.
- Upload/DB failure marks asset/job failed as appropriate and queues orphan cleanup. No DB row references an object that was not uploaded.

## Specification source

M3. Asset And Media Library Module; Responsibilities And Dependencies; Required asset-file purposes; Object-key policy; Boundary rules; Transaction And Consistency Policy (ingest Compute artifact); Sequence Diagram 1; M3 public API.

## Dependencies

- T02, T04, T06.
- httpx or read-only Compute adapter, hashlib, pathlib, boto3/MinIO S3 API, SQLAlchemy transaction.
- Depends on selected artifact IDs and immutable output spec from T06. Produces media event consumed by T08.
- S3 is non-transactional; orphan cleanup uses T04 event delivery.

## Test plan

~~~bash
docker compose -f docker-compose.dev.yml up --build -d
curl --noproxy '*' -sS -f -b /tmp/studio.cookie -H 'Idempotency-Key: ingest-job-0001' -H 'Content-Type: application/json' -d '{"recipeId":"RECIPE_ID","output":{"kind":"image","format":"png","width":512,"height":512}}' http://localhost:8000/v1/render-jobs
curl --noproxy '*' -sS -f -b /tmp/studio.cookie http://localhost:8000/v1/render-jobs/RENDER_JOB_ID
curl --noproxy '*' -sS -f -b /tmp/studio.cookie http://localhost:8000/v1/me/assets/ASSET_ID
~~~

Configure the Compute stub to return an allowed PNG, then assert completed job, ready private asset, master metadata without object key, and no public master URL. Repeat with path traversal, wrong extension, checksum mismatch, and MinIO upload failure; assert job/asset failure and no ready asset or download URL.

## Implementation plan

1. Define M3 ports and repository methods for processing creation, locked finalization, and orphan cleanup.
2. Implement strict artifact selector/reader and streaming size/hash validation.
3. Implement private object upload, generated provenance manifest, and S3 error classification.
4. Use two short DB transactions around external I/O; enqueue cleanup on every orphan path.
5. Expose only safe AssetView mapping; leave derivatives and signed download to T08.
