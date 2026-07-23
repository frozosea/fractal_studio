# T05 — M2 Immutable Recipes and Bounded Synchronous Preview

## Task description

Implement canonical fractal recipe persistence and the bounded, non-persistent preview route. Preview validates a small request, rate-limits it in Redis, maps the canonical structure through a pure versioned mapper, calls private Compute synchronously, and converts its RGBA8 response to PNG in memory.

## Work scope

- app/studio/{router.py,models.py,recipe_service.py,recipe_repository.py,preview_service.py,compute_request_mapper.py,rgba_png_encoder.py,quota_service.py}
- app/infrastructure/{compute/compute_client.py,redis/quota_store.py}
- tests/e2e/test_recipes_preview.py

## Goal

Persist an immutable, deduplicated canonical structure and offer safe low-cost previews without creating a recipe, render job, asset, S3 object, or outbox event.

## Acceptance criteria

- POST /v1/recipes canonicalizes before SHA-256; same owner and canonical hash returns the existing recipe (200), otherwise 201 with an audit event committed in the same transaction. Recipes have no PATCH route.
- POST /v1/studio/preview accepts only configured dimensions up to 1024x1024 and 1,048,576 pixels, has a five-second Compute timeout, and returns image/png.
- Preview writes only its Redis rate-limit counter. Redis failure is fail-closed; preview never writes durable product state.
- Compute mapper is pure/versioned and Compute client sends private Bearer service credentials without exposing them to browser/logs.

## Specification source

M2. Studio And Render Job Module; Responsibilities And Dependencies; Verified Current C++ Compute Contract; Transaction And Consistency Policy (save recipe); MVP Technical Limits; Request And Response Models; M2. Studio And Render Jobs.

## Dependencies

- T01, T02, T03.
- Pydantic v2, hashlib, canonical JSON/orjson, redis.asyncio, httpx.AsyncClient, Pillow.
- Compute contract stub implements POST /api/map/render-inline with RGBA8 bytes and X-FSD-width/X-FSD-height; production dependency is compute-openapi.yaml.
- Does not depend on outbox; durable rendering is T06.

## Test plan

~~~bash
docker compose -f docker-compose.dev.yml up --build -d
curl --noproxy '*' -sS -f -c /tmp/studio.cookie -H 'Content-Type: application/json' -d '{"email":"studio@example.test","password":"correct-horse-01"}' http://localhost:8000/v1/auth/register
curl --noproxy '*' -sS -f -b /tmp/studio.cookie -H 'Idempotency-Key: recipe-0001' -H 'Content-Type: application/json' -d '{"canonicalSpec":{"version":1,"seed":42}}' http://localhost:8000/v1/recipes
curl --noproxy '*' -sS -f -b /tmp/studio.cookie http://localhost:8000/v1/me/recipes
curl --noproxy '*' -sS -f -b /tmp/studio.cookie -H 'Content-Type: application/json' -o /tmp/preview.png -d '{"canonicalSpec":{"version":1,"seed":42},"width":512,"height":512}' http://localhost:8000/v1/studio/preview
curl --noproxy '*' -sS -o /dev/null -w '%{http_code}\n' -b /tmp/studio.cookie -H 'Content-Type: application/json' -d '{"canonicalSpec":{"version":1},"width":4096,"height":4096}' http://localhost:8000/v1/studio/preview
~~~

Assert first recipe is 201, list is a cursor collection envelope, same canonical payload with another key is 200 and same ID, preview is a valid PNG, oversized preview is 422, and repeated preview reaches 429. Verify no asset/job appears through GET /v1/me/assets, no object is publicly reachable, and the accepted recipe has one request-correlated audit event.

## Implementation plan

1. Define strict recipe/preview DTOs and canonical JSON validation.
2. Implement recipe repository/service with owner/hash uniqueness, cursor listing, and transaction-bound audit write.
3. Implement Redis atomic preview limiter with fail-closed behavior.
4. Implement mapper and typed Compute inline adapter; validate metadata and encode RGBA8 with Pillow.
5. Map validation, quota, Compute timeout, and malformed-frame failures to documented HTTP errors.
6. Register routes and request-correlated logs without canonical secret data.
