# T09 — M4 Catalogue, Listings, Licences, and Favourites

## Task description

Implement public exploration, listing draft/edit/publish/unpublish, immutable listing versions, a server-owned licence registry, and user-to-asset favourites. M4 reads safe asset data through the M3 port and never reads masters or calls Compute/Alipay.

## Work scope

- app/marketplace/{router.py,service.py,repository.py,models.py,ports.py,licence_registry.py}
- app/assets/ports.py, app/main.py
- migrations/versions/ follow-up indexes only if T02 did not include them
- tests/e2e/test_marketplace.py

## Goal

Publish only ready creator-owned assets with complete preview derivatives into a secure searchable catalogue; freeze buyer-facing content and licence terms at publication.

## Acceptance criteria

- GET /v1/explore returns published-only ListingView projections, default newest 24, cursor pagination, normalized filter-bound cursors, and no master key/original URL.
- Draft edit is allowed only to its creator. Publish locks/revalidates listing, requires owned ready private publishable asset, one active licence, valid CNY Decimal price, and creates next immutable ListingVersion/current pointer atomically.
- State path is draft -> published -> unpublished -> draft; every publish creates a new immutable version.
- One active licence offer and one non-archived listing per asset are enforced. Favourites are unique user/asset bookmarks and do not affect rank.
- Search uses PostgreSQL full-text and pg_trgm with B-tree/cursor indexes; no recommendation endpoint is added.

## Specification source

M4. Catalogue Listing And Licence Module; Responsibilities And Dependencies; MVP Catalogue Feed; Boundary rules; Transaction And Consistency Policy (publish listing); Public API M4; Listing publish sequence diagram.

## Dependencies

- T02, T03, T08.
- SQLAlchemy async, Pydantic v2, orjson, PostgreSQL FTS/pg_trgm.
- M3 AssetReader port: findOwnedAsset, findPublicPreview, findPublishableAsset. T11 uses M4 ListingSnapshotReader port.
- Publish transaction locks the draft; licence snapshots are immutable and cannot later be rewritten.

## Test plan

~~~bash
docker compose -f docker-compose.dev.yml up --build -d
curl --noproxy '*' -sS -f -b /tmp/studio.cookie -H 'Idempotency-Key: listing-0001' -H 'Content-Type: application/json' -d '{"assetId":"ASSET_ID","title":"Orbit","description":"Test fractal","tags":["orbit"],"price":"19.90","licenceOffer":{"code":"personal","termsVersion":"v1"}}' http://localhost:8000/v1/listings
curl --noproxy '*' -sS -f -b /tmp/studio.cookie -H 'Idempotency-Key: publish-0001' -X POST http://localhost:8000/v1/listings/LISTING_ID/publish
curl --noproxy '*' -sS -f 'http://localhost:8000/v1/explore?sort=newest&limit=24'
curl --noproxy '*' -sS -f http://localhost:8000/v1/listings/LISTING_ID
curl --noproxy '*' -sS -f -b /tmp/studio.cookie 'http://localhost:8000/v1/me/listings?status=published'
curl --noproxy '*' -sS -f -b /tmp/studio.cookie -H 'Idempotency-Key: listing-edit-0001' -H 'Content-Type: application/json' -X PATCH -d '{"title":"Orbit revised"}' http://localhost:8000/v1/listings/LISTING_ID
curl --noproxy '*' -sS -f -b /tmp/buyer.cookie -H 'Idempotency-Key: favourite-0001' -X POST http://localhost:8000/v1/assets/ASSET_ID/favorite
curl --noproxy '*' -sS -f -b /tmp/buyer.cookie http://localhost:8000/v1/me/favorites
curl --noproxy '*' -sS -f -b /tmp/buyer.cookie -H 'Idempotency-Key: favourite-delete-0001' -X DELETE http://localhost:8000/v1/assets/ASSET_ID/favorite
curl --noproxy '*' -sS -f -b /tmp/studio.cookie -H 'Idempotency-Key: unpublish-0001' -X POST http://localhost:8000/v1/listings/LISTING_ID/unpublish
~~~

Assert draft 201, publish 200, public/owner detail and creator listing reads return safe views, explore exposes public preview but no master URL, and favourite 201 then replay-safe delete 204. Try an unready, foreign, hidden, or missing-derivative asset: publish must fail. Unpublish, edit while draft, publish again, and verify a new version is used while prior snapshot remains returned by later order records.

## Implementation plan

1. Define DTOs, Money Decimal validation, immutable licence registry, and safe projections.
2. Implement repository search/cursor encoding that includes normalized filters and rejects mismatch with 422.
3. Implement favourites and creator draft CRUD with idempotency.
4. Implement locked publish/unpublish with M3 port validation, version snapshot, licence terms, tags, and audit event.
5. Implement public/owner detail authorization and SQL indexes.
