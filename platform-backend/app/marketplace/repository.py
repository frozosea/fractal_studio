"""M4 SQL only. This module never accesses storage, Compute, or payments."""

from __future__ import annotations

import json
from dataclasses import dataclass
from datetime import datetime
from decimal import Decimal
from typing import Any, Literal
from uuid import UUID, uuid4

from sqlalchemy import text
from sqlalchemy.ext.asyncio import AsyncConnection

from app.marketplace.ports import PublishedOfferSnapshot


@dataclass(frozen=True, slots=True)
class ListingRecord:
    id: UUID
    asset_id: UUID
    creator_id: UUID
    creator_handle: str
    creator_display_name: str
    status: str
    title: str
    description: str
    tags: list[str]
    price: Decimal
    currency: str
    created_at: datetime
    published_at: datetime | None
    current_published_version_id: UUID | None
    licence_offer_id: UUID
    licence_code: str
    licence_terms_version: str
    licence_terms: dict[str, object]
    variant: str | None = None
    iterations: int | None = None
    output_width: int | None = None
    output_height: int | None = None
    color_map: str | None = None
    color_mode: str | None = None
    view_scale: float | None = None
    cursor_value: object | None = None


@dataclass(frozen=True, slots=True)
class FavoriteRecord:
    asset_id: UUID
    created_at: datetime
    listing: ListingRecord | None


def _record(row: Any) -> ListingRecord:
    data = dict(row)
    return ListingRecord(
        id=data["id"],
        asset_id=data["asset_id"],
        creator_id=data["creator_id"],
        creator_handle=str(data["creator_handle"]),
        creator_display_name=str(data["creator_display_name"]),
        status=str(data["status"]),
        title=str(data["title"]),
        description=str(data["description"]),
        tags=list(data["tags"] or []),
        price=Decimal(data["price_amount"]),
        currency=str(data["currency"]),
        created_at=data["created_at"],
        published_at=data["published_at"],
        current_published_version_id=data["current_published_version_id"],
        licence_offer_id=data["licence_offer_id"],
        licence_code=str(data["licence_code"]),
        licence_terms_version=str(data["licence_terms_version"]),
        licence_terms=dict(data["licence_terms"]),
        variant=data.get("variant"),
        iterations=data.get("iterations"),
        output_width=data.get("output_width"),
        output_height=data.get("output_height"),
        color_map=data.get("color_map"),
        color_mode=data.get("color_mode"),
        view_scale=data.get("view_scale"),
        cursor_value=data.get("cursor_value"),
    )


# Facets a buyer browses by, resolved from the two JSONB blobs that hold them.
# Kept in step with `_FACET_EXPRESSIONS` in
# migrations/versions/20260730_0018_listing_render_facets.py, which backfills
# rows that predate the columns. Alembic revisions stay self-contained, so the
# two copies are deliberate.
#
# `canonical_spec` is dumped with exclude_none=True, so `colorMap`,
# `colorProgram` and `orbitProgram` are absent rather than null — hence
# jsonb_exists rather than a null check. `colorMap` and `colorProgram` are
# mutually exclusive by validator, so the custom gradient can safely take over
# the same column.
_FACET_DERIVATION = """
    CASE WHEN jsonb_exists(r.canonical_spec, 'orbitProgram') THEN 'custom'
         ELSE r.canonical_spec ->> 'variant' END,
    COALESCE((j.output_spec_json ->> 'iterations')::int,
             (r.canonical_spec ->> 'iterations')::int),
    COALESCE((j.output_spec_json ->> 'width')::int,
             (j.output_spec_json ->> 'resolution')::int),
    COALESCE((j.output_spec_json ->> 'height')::int,
             (j.output_spec_json ->> 'resolution')::int),
    CASE WHEN jsonb_exists(r.canonical_spec, 'colorProgram') THEN 'custom_gradient'
         ELSE r.canonical_spec ->> 'colorMap' END,
    r.canonical_spec ->> 'colorMode',
    (r.canonical_spec ->> 'scale')::double precision
"""

# Iteration depth and pixel count are browsed in bands rather than as free
# ranges. The bands are applied over the raw columns instead of being stored, so
# thresholds can be retuned without a migration. Upper bound `None` marks the
# open-ended top band.
_DEPTH_EXPR = "l.iterations"
_DEPTH_BANDS: tuple[tuple[str, int | None], ...] = (
    ("le512", 512),
    ("le1024", 1024),
    ("le2048", 2048),
    ("gt2048", None),
)
_RESOLUTION_EXPR = "l.output_width * l.output_height"
_RESOLUTION_BANDS: tuple[tuple[str, int | None], ...] = (
    ("le1mp", 1_000_000),
    ("le2mp", 2_000_000),
    ("le8mp", 8_000_000),
    ("gt8mp", None),
)


def _band_predicates(expr: str, bands: tuple[tuple[str, int | None], ...]) -> dict[str, str]:
    """One WHERE fragment per band, derived from the shared boundaries."""
    predicates: dict[str, str] = {}
    previous: int | None = None
    for key, upper in bands:
        if upper is None:
            predicates[key] = f"{expr} > {previous}"
        elif previous is None:
            predicates[key] = f"{expr} <= {upper}"
        else:
            predicates[key] = f"{expr} > {previous} AND {expr} <= {upper}"
        previous = upper
    return predicates


def _band_case(expr: str, bands: tuple[tuple[str, int | None], ...]) -> str:
    """The same boundaries as a CASE, for counting. Earlier arms win."""
    arms = [
        f"WHEN {expr} <= {upper} THEN '{key}'" if upper is not None else f"ELSE '{key}'"
        for key, upper in bands
    ]
    return "CASE " + " ".join(arms) + " END"


# Keys are validated as a Literal in the router before reaching these dicts, so
# the fragments are never caller-controlled.
DEPTH_BUCKETS: dict[str, str] = _band_predicates(_DEPTH_EXPR, _DEPTH_BANDS)
RESOLUTION_BUCKETS: dict[str, str] = _band_predicates(_RESOLUTION_EXPR, _RESOLUTION_BANDS)
DEPTH_KEYS = tuple(key for key, _ in _DEPTH_BANDS)
RESOLUTION_KEYS = tuple(key for key, _ in _RESOLUTION_BANDS)


_DETAIL_SELECT = """
    SELECT l.id, l.asset_id, l.creator_id, l.status::text AS status, l.title, l.description,
           l.price_amount, l.currency, l.created_at, l.published_at, l.current_published_version_id,
           cp.handle AS creator_handle, cp.display_name AS creator_display_name,
           lo.id AS licence_offer_id, lo.code AS licence_code,
           lo.terms_version AS licence_terms_version, lo.terms_json AS licence_terms,
           l.variant, l.iterations, l.output_width, l.output_height,
           l.color_map, l.color_mode, l.view_scale,
           COALESCE((SELECT array_agg(lt.tag ORDER BY lt.tag) FROM listing_tags lt
                     WHERE lt.listing_id = l.id), ARRAY[]::text[]) AS tags
    FROM listings l
    JOIN creator_profiles cp ON cp.user_id = l.creator_id
    JOIN licence_offers lo ON lo.listing_id = l.id AND lo.is_active
"""


async def create_draft(
    connection: AsyncConnection,
    *,
    asset_id: UUID,
    creator_id: UUID,
    title: str,
    description: str,
    tags: list[str],
    price: Decimal,
    licence_code: str,
    licence_terms_version: str,
    licence_terms: dict[str, object],
) -> ListingRecord:
    listing_id, offer_id = uuid4(), uuid4()
    await connection.execute(
        text(
            f"""
            INSERT INTO listings (id, asset_id, creator_id, status, title, description, price_amount, currency,
                                  variant, iterations, output_width, output_height, color_map, color_mode, view_scale)
            SELECT :id, :asset_id, :creator_id, 'draft', :title, :description, :price, 'CNY',
                   {_FACET_DERIVATION}
            FROM assets a
            JOIN fractal_recipes r ON r.id = a.recipe_id
            JOIN render_jobs j ON j.id = a.render_job_id
            WHERE a.id = :asset_id
            """
        ),
        {"id": listing_id, "asset_id": asset_id, "creator_id": creator_id, "title": title,
         "description": description, "price": price},
    )
    await connection.execute(
        text(
            """
            INSERT INTO licence_offers (id, listing_id, code, terms_version, terms_json, is_active)
            VALUES (:id, :listing_id, :code, :terms_version, CAST(:terms AS jsonb), true)
            """
        ),
        {"id": offer_id, "listing_id": listing_id, "code": licence_code,
         "terms_version": licence_terms_version, "terms": json.dumps(licence_terms)},
    )
    await replace_tags(connection, listing_id=listing_id, tags=tags)
    record = await find_owned(connection, listing_id=listing_id, creator_id=creator_id)
    assert record is not None
    return record


async def replace_tags(connection: AsyncConnection, *, listing_id: UUID, tags: list[str]) -> None:
    await connection.execute(text("DELETE FROM listing_tags WHERE listing_id = :listing_id"), {"listing_id": listing_id})
    if tags:
        await connection.execute(
            text("INSERT INTO listing_tags (listing_id, tag) SELECT :listing_id, unnest(CAST(:tags AS text[]))"),
            {"listing_id": listing_id, "tags": tags},
        )


async def lock_owned(connection: AsyncConnection, *, listing_id: UUID, creator_id: UUID) -> ListingRecord | None:
    # Lock aggregate first; detail is then safe to read in same transaction.
    locked = await connection.scalar(
        text("SELECT id FROM listings WHERE id = :listing_id AND creator_id = :creator_id FOR UPDATE"),
        {"listing_id": listing_id, "creator_id": creator_id},
    )
    if locked is None:
        return None
    return await find_owned(connection, listing_id=listing_id, creator_id=creator_id)


async def update_draft(
    connection: AsyncConnection,
    *,
    listing: ListingRecord,
    title: str | None,
    description: str | None,
    tags: list[str] | None,
    price: Decimal | None,
    licence: tuple[str, str, dict[str, object]] | None,
) -> ListingRecord:
    # `unpublished -> draft` happens only as part of an owner edit; no endpoint exposes mutable
    # unpublished content publicly.
    await connection.execute(
        text(
            """
            UPDATE listings SET status = 'draft', title = :title, description = :description,
              price_amount = :price, published_at = NULL, current_published_version_id = NULL
            WHERE id = :id
            """
        ),
        {"id": listing.id, "title": title if title is not None else listing.title,
         "description": description if description is not None else listing.description,
         "price": price if price is not None else listing.price},
    )
    if tags is not None:
        await replace_tags(connection, listing_id=listing.id, tags=tags)
    if licence is not None:
        code, terms_version, terms = licence
        await connection.execute(
            text("UPDATE licence_offers SET is_active = false WHERE listing_id = :listing_id AND is_active"),
            {"listing_id": listing.id},
        )
        await connection.execute(
            text(
                """
                INSERT INTO licence_offers (id, listing_id, code, terms_version, terms_json, is_active)
                VALUES (:id, :listing_id, :code, :terms_version, CAST(:terms AS jsonb), true)
                """
            ),
            {"id": uuid4(), "listing_id": listing.id, "code": code,
             "terms_version": terms_version, "terms": json.dumps(terms)},
        )
    updated = await find_owned(connection, listing_id=listing.id, creator_id=listing.creator_id)
    assert updated is not None
    return updated


async def rename_published(
    connection: AsyncConnection, *, listing: ListingRecord, title: str
) -> ListingRecord:
    # Title is the only field a published listing may change in place; the live row and the
    # current published version snapshot move together so market, detail and favourites agree.
    await connection.execute(
        text("UPDATE listings SET title = :title WHERE id = :id AND status = 'published'"),
        {"id": listing.id, "title": title},
    )
    await connection.execute(
        text(
            """
            UPDATE listing_versions
            SET snapshot_json = jsonb_set(snapshot_json, '{title}', to_jsonb(CAST(:title AS text)))
            WHERE id = :version_id
            """
        ),
        {"version_id": listing.current_published_version_id, "title": title},
    )
    renamed = await find_published(connection, listing_id=listing.id)
    assert renamed is not None
    return renamed


async def publish(
    connection: AsyncConnection, *, listing: ListingRecord, snapshot: dict[str, object]
) -> ListingRecord:
    latest = await connection.scalar(
        text("SELECT COALESCE(MAX(version), 0) FROM listing_versions WHERE listing_id = :listing_id"),
        {"listing_id": listing.id},
    )
    version_id = uuid4()
    now = await connection.scalar(text("SELECT now()"))
    await connection.execute(
        text(
            """
            INSERT INTO listing_versions (id, listing_id, version, snapshot_json, published_at)
            VALUES (:id, :listing_id, :version, CAST(:snapshot AS jsonb), :published_at)
            """
        ),
        {"id": version_id, "listing_id": listing.id, "version": int(latest) + 1,
         "snapshot": json.dumps(snapshot), "published_at": now},
    )
    await connection.execute(
        text(
            """
            UPDATE listings SET status = 'published', published_at = :published_at,
              current_published_version_id = :version_id
            WHERE id = :listing_id AND status = 'draft'
            """
        ),
        {"listing_id": listing.id, "published_at": now, "version_id": version_id},
    )
    published = await find_published(connection, listing_id=listing.id)
    assert published is not None
    return published


async def unpublish(connection: AsyncConnection, *, listing: ListingRecord) -> ListingRecord:
    await connection.execute(
        text(
            """
            UPDATE listings SET status = 'unpublished', published_at = NULL, current_published_version_id = NULL
            WHERE id = :listing_id AND status = 'published'
            """
        ),
        {"listing_id": listing.id},
    )
    unpublished = await find_owned(connection, listing_id=listing.id, creator_id=listing.creator_id)
    assert unpublished is not None
    return unpublished


async def find_owned(
    connection: AsyncConnection, *, listing_id: UUID, creator_id: UUID
) -> ListingRecord | None:
    """Archived listings read as absent: withdrawing one is terminal."""
    row = await connection.execute(
        text(_DETAIL_SELECT + " WHERE l.id = :listing_id AND l.creator_id = :creator_id AND l.status <> 'archived'"),
        {"listing_id": listing_id, "creator_id": creator_id},
    )
    found = row.mappings().one_or_none()
    return _record(found) if found is not None else None


async def find_published(connection: AsyncConnection, *, listing_id: UUID) -> ListingRecord | None:
    row = await connection.execute(
        text(_DETAIL_SELECT + " WHERE l.id = :listing_id AND l.status = 'published'"), {"listing_id": listing_id}
    )
    found = row.mappings().one_or_none()
    return _record(found) if found is not None else None


async def list_creator(
    connection: AsyncConnection,
    *,
    creator_id: UUID,
    status: str | None,
    limit: int,
    before_created_at: datetime | None,
    before_id: UUID | None,
) -> list[ListingRecord]:
    # Archived listings were withdrawn (the asset was hidden or deleted); they
    # are gone from the creator's shelf, not a state they can act on.
    predicate = "l.creator_id = :creator_id AND l.status <> 'archived'"
    params: dict[str, object] = {"creator_id": creator_id, "limit": limit}
    if status is not None:
        predicate += " AND l.status::text = :status"
        params["status"] = status
    if before_created_at is not None and before_id is not None:
        predicate += " AND (l.created_at, l.id) < (:before_at, :before_id)"
        params.update({"before_at": before_created_at, "before_id": before_id})
    rows = await connection.execute(
        text(_DETAIL_SELECT + f" WHERE {predicate} ORDER BY l.created_at DESC, l.id DESC LIMIT :limit"),
        params,
    )
    return [_record(row) for row in rows.mappings()]


async def search_published(
    connection: AsyncConnection,
    *,
    q: str | None,
    tag: str | None,
    creator: str | None,
    creator_exact: str | None = None,
    media_type: str | None,
    min_price: Decimal | None,
    max_price: Decimal | None,
    variant: str | None = None,
    color_map: str | None = None,
    depth: str | None = None,
    resolution: str | None = None,
    sort: Literal["newest", "price_asc", "price_desc", "relevance"],
    after: dict[str, object] | None,
    limit: int,
) -> list[ListingRecord]:
    predicates = ["l.status = 'published'", "l.current_published_version_id IS NOT NULL"]
    params: dict[str, object] = {"limit": limit}
    if q:
        predicates.append(
            "(to_tsvector('simple', concat_ws(' ', l.title, l.description, cp.handle, cp.display_name, "
            "COALESCE((SELECT string_agg(lt.tag, ' ') FROM listing_tags lt WHERE lt.listing_id = l.id), ''))) "
            "@@ websearch_to_tsquery('simple', :q) "
            "OR l.title % :q OR cp.handle % :q OR cp.display_name % :q)"
        )
        params["q"] = q
    if tag:
        predicates.append("EXISTS (SELECT 1 FROM listing_tags lt WHERE lt.listing_id = l.id AND lt.tag = :tag)")
        params["tag"] = tag
    if creator:
        # Creator chips come from exact handle counts. Keep filtering exact as
        # well, or selecting `bob` would also return `bobby` and contradict the
        # count displayed by the facet.
        predicates.append("cp.handle = :creator")
        params["creator"] = creator
    if creator_exact:
        # Profile routes keep a separate filter field because their cursor
        # shape differs from the public catalogue's facet query.
        predicates.append("cp.handle = :creator_exact")
        params["creator_exact"] = creator_exact
    if media_type:
        predicates.append("a.media_type::text = :media_type")
        params["media_type"] = media_type
    if min_price is not None:
        predicates.append("l.price_amount >= :min_price")
        params["min_price"] = min_price
    if max_price is not None:
        predicates.append("l.price_amount <= :max_price")
        params["max_price"] = max_price
    if variant:
        predicates.append("l.variant = :variant")
        params["variant"] = variant
    if color_map:
        predicates.append("l.color_map = :color_map")
        params["color_map"] = color_map
    if depth and depth in DEPTH_BUCKETS:
        predicates.append(f"({DEPTH_BUCKETS[depth]})")
    if resolution and resolution in RESOLUTION_BUCKETS:
        predicates.append(f"({RESOLUTION_BUCKETS[resolution]})")

    # The rank vector has to list the same columns as the filter above, or a
    # row can match the WHERE clause and then score zero when sorting by
    # relevance.
    rank = (
        "ts_rank(to_tsvector('simple', concat_ws(' ', l.title, l.description, cp.handle, cp.display_name)), "
        "websearch_to_tsquery('simple', :q))"
    )
    cursor_expr = "l.published_at"
    if sort == "price_asc":
        cursor_expr, order, comparison = "l.price_amount", "l.price_amount ASC, l.id ASC", ">"
    elif sort == "price_desc":
        cursor_expr, order, comparison = "l.price_amount", "l.price_amount DESC, l.id DESC", "<"
    elif sort == "relevance" and q:
        cursor_expr, order, comparison = rank, f"{rank} DESC, l.id DESC", "<"
    else:
        order, comparison = "l.published_at DESC, l.id DESC", "<"
    if after is not None:
        predicates.append(f"({cursor_expr}, l.id) {comparison} (:after_value, :after_id)")
        params["after_value"] = after["value"]
        params["after_id"] = after["id"]

    rows = await connection.execute(
        text(
            _DETAIL_SELECT.replace("FROM listings l", f", {cursor_expr} AS cursor_value FROM listings l")
            + " JOIN assets a ON a.id = l.asset_id"
            + " WHERE " + " AND ".join(predicates) + f" ORDER BY {order} LIMIT :limit"
        ),
        params,
    )
    return [_record(row) for row in rows.mappings()]


_PUBLISHED = "l.status = 'published' AND l.current_published_version_id IS NOT NULL"


async def facet_counts(connection: AsyncConnection) -> list[dict[str, object]]:
    """
    Values actually present in the published catalogue, with how many listings
    carry each. Lets the browser show only facets that would return something
    rather than a wall of empty filters.
    """
    rows = await connection.execute(
        text(
            f"""
            SELECT 'creator' AS facet, cp.handle AS value, COUNT(*) AS total
            FROM listings l JOIN creator_profiles cp ON cp.user_id = l.creator_id
            WHERE {_PUBLISHED}
            GROUP BY cp.handle
            UNION ALL
            SELECT 'variant' AS facet, l.variant AS value, COUNT(*) AS total
            FROM listings l WHERE {_PUBLISHED} AND l.variant IS NOT NULL
            GROUP BY l.variant
            UNION ALL
            SELECT 'colorMap', l.color_map, COUNT(*)
            FROM listings l WHERE {_PUBLISHED} AND l.color_map IS NOT NULL
            GROUP BY l.color_map
            UNION ALL
            SELECT 'depth', {_band_case(_DEPTH_EXPR, _DEPTH_BANDS)}, COUNT(*)
            FROM listings l WHERE {_PUBLISHED} AND l.iterations IS NOT NULL
            GROUP BY 2
            UNION ALL
            SELECT 'resolution', {_band_case(_RESOLUTION_EXPR, _RESOLUTION_BANDS)}, COUNT(*)
            FROM listings l
            WHERE {_PUBLISHED} AND l.output_width IS NOT NULL AND l.output_height IS NOT NULL
            GROUP BY 2
            ORDER BY 1, 3 DESC, 2
            """
        )
    )
    return [
        {"facet": str(row["facet"]), "value": str(row["value"]), "count": int(row["total"])}
        for row in rows.mappings()
    ]


async def save_favorite(connection: AsyncConnection, *, user_id: UUID, asset_id: UUID) -> FavoriteRecord | None:
    inserted = await connection.execute(
        text(
            """
            INSERT INTO favorites (user_id, asset_id)
            SELECT :user_id, :asset_id
            WHERE EXISTS (SELECT 1 FROM listings WHERE asset_id = :asset_id AND status = 'published')
            ON CONFLICT (user_id, asset_id) DO NOTHING
            RETURNING created_at
            """
        ),
        {"user_id": user_id, "asset_id": asset_id},
    )
    created_at = inserted.scalar_one_or_none()
    if created_at is None:
        existing = await connection.scalar(
            text("SELECT created_at FROM favorites WHERE user_id = :user_id AND asset_id = :asset_id"),
            {"user_id": user_id, "asset_id": asset_id},
        )
        if existing is None:
            return None
        created_at = existing
    listing = await _published_by_asset(connection, asset_id=asset_id)
    return FavoriteRecord(asset_id=asset_id, created_at=created_at, listing=listing)


async def delete_favorite(connection: AsyncConnection, *, user_id: UUID, asset_id: UUID) -> None:
    await connection.execute(
        text("DELETE FROM favorites WHERE user_id = :user_id AND asset_id = :asset_id"),
        {"user_id": user_id, "asset_id": asset_id},
    )


async def list_favorites(
    connection: AsyncConnection, *, user_id: UUID, limit: int, before: tuple[datetime, UUID] | None
) -> list[FavoriteRecord]:
    predicate = "f.user_id = :user_id"
    params: dict[str, object] = {"user_id": user_id, "limit": limit}
    if before is not None:
        predicate += " AND (f.created_at, f.asset_id) < (:before_at, :before_asset_id)"
        params.update({"before_at": before[0], "before_asset_id": before[1]})
    favorite_rows = await connection.execute(
        text(f"SELECT f.asset_id, f.created_at FROM favorites f WHERE {predicate} ORDER BY f.created_at DESC, f.asset_id DESC LIMIT :limit"),
        params,
    )
    result: list[FavoriteRecord] = []
    for row in favorite_rows.mappings():
        result.append(FavoriteRecord(asset_id=row["asset_id"], created_at=row["created_at"], listing=await _published_by_asset(connection, asset_id=row["asset_id"])))
    return result


async def _published_by_asset(connection: AsyncConnection, *, asset_id: UUID) -> ListingRecord | None:
    row = await connection.execute(
        text(_DETAIL_SELECT + " WHERE l.asset_id = :asset_id AND l.status = 'published'"), {"asset_id": asset_id}
    )
    found = row.mappings().one_or_none()
    return _record(found) if found is not None else None


async def find_published_offer(
    connection: AsyncConnection, *, listing_id: UUID, licence_offer_id: UUID
) -> PublishedOfferSnapshot | None:
    row = await connection.execute(
        text(
            """
            SELECT l.id AS listing_id, l.asset_id, l.creator_id, l.price_amount, l.currency,
                   v.id AS listing_version_id, v.snapshot_json, lo.id AS licence_offer_id, lo.terms_json
            FROM listings l
            JOIN listing_versions v ON v.id = l.current_published_version_id
            JOIN licence_offers lo ON lo.id = :licence_offer_id AND lo.listing_id = l.id AND lo.is_active
            WHERE l.id = :listing_id AND l.status = 'published'
            """
        ),
        {"listing_id": listing_id, "licence_offer_id": licence_offer_id},
    )
    found = row.mappings().one_or_none()
    if found is None:
        return None
    return PublishedOfferSnapshot(
        listing_id=found["listing_id"], listing_version_id=found["listing_version_id"],
        licence_offer_id=found["licence_offer_id"], asset_id=found["asset_id"], creator_id=found["creator_id"],
        price=Decimal(found["price_amount"]), currency=str(found["currency"]),
        listing_snapshot=dict(found["snapshot_json"]), licence_snapshot=dict(found["terms_json"]),
    )
