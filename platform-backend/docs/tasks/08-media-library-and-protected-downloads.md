# T08 — M3 Media Derivatives, Private Library, and Protected Downloads

## Task description

Complete M3 after master ingestion: create thumbnails, watermarked previews, and video posters asynchronously; expose the private asset library; hide or soft-delete assets safely; issue five-minute signed master URLs only to owners or active entitlement holders.

## Work scope

- app/assets/{router.py,service.py,repository.py,media_worker.py,cleanup_service.py,models.py,ports.py}
- app/infrastructure/storage/object_storage.py
- app/outbox/{service.py,worker.py}
- tests/e2e/test_asset_library.py

## Goal

Make derived public previews available without weakening master-object authorization or deleting files required by paid orders.

## Acceptance criteria

- Media worker handles media.create_derivatives.v1 idempotently and adds thumbnail, watermarked_preview, and video_poster only where applicable.
- GET /v1/me/assets and GET /v1/me/assets/{assetId} return only owner-safe metadata; no Compute ID/path, S3 key, raw original URL, or manifest details.
- PATCH hide/restore is allowed only for unlisted assets. DELETE is a soft deletion; physical master removal is blocked while any order item or active entitlement references it.
- POST /v1/assets/{assetId}/download-url requires owner or M5 EntitlementReader approval and returns a five-minute signed master URL. Public catalogue reads only preview derivatives.
- Cleanup removes expired orphan/eligible objects without violating retained purchased-master policy.

## Specification source

M3. Asset And Media Library Module; Responsibilities And Dependencies; Required asset-file purposes; Boundary rules; M7 Event routing; Transaction And Consistency Policy (ingest); M3. Private Asset Library; Protected Download sequence diagram.

## Dependencies

- T02, T04, T07.
- Pillow and system ffmpeg, boto3/MinIO, asyncio worker temporary directory.
- M5 EntitlementReader port is implemented by T11; before T11, owner-only path can be tested but purchaser download acceptance waits for T11.
- Derivative writes use asset row lock/idempotent file-purpose uniqueness.

## Test plan

~~~bash
docker compose -f docker-compose.dev.yml up --build -d
curl --noproxy '*' -sS -f -b /tmp/studio.cookie http://localhost:8000/v1/me/assets
curl --noproxy '*' -sS -f -b /tmp/studio.cookie -H 'Idempotency-Key: asset-hide-0001' -H 'Content-Type: application/json' -X PATCH -d '{"visibility":"hidden"}' http://localhost:8000/v1/me/assets/ASSET_ID
curl --noproxy '*' -sS -f -b /tmp/studio.cookie -X POST http://localhost:8000/v1/assets/ASSET_ID/download-url
curl --noproxy '*' -sS -o /dev/null -w '%{http_code}\n' -b /tmp/other.cookie -X POST http://localhost:8000/v1/assets/ASSET_ID/download-url
~~~

Create a completed image and video through T06/T07. Assert derivative previews appear after worker processing, owner download is 200 with five-minute expiry, unrelated user gets 403, and AssetView has no storage/Compute internals. Publish an asset after T09, then assert hide fails; create purchase after T11, soft-delete as owner, and assert buyer can still download.

## Implementation plan

1. Implement MediaWorker with bounded temp storage, MIME validation, Pillow transforms, ffmpeg invocation, and cleanup.
2. Finalize derivative file records under row lock and public/previews key policy.
3. Implement owner library cursor reads, visibility transition checks, and soft-delete/retention rules.
4. Implement entitlement port invocation and signed URL adapter with exact TTL.
5. Add scheduled cleanup event handling and safe retry semantics.
