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
        cursor_value=data.get("cursor_value"),
    )


_DETAIL_SELECT = """
    SELECT l.id, l.asset_id, l.creator_id, l.status::text AS status, l.title, l.description,
           l.price_amount, l.currency, l.created_at, l.published_at, l.current_published_version_id,
           cp.handle AS creator_handle, cp.display_name AS creator_display_name,
           lo.id AS licence_offer_id, lo.code AS licence_code,
           lo.terms_version AS licence_terms_version, lo.terms_json AS licence_terms,
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
            """
            INSERT INTO listings (id, asset_id, creator_id, status, title, description, price_amount, currency)
            VALUES (:id, :asset_id, :creator_id, 'draft', :title, :description, :price, 'CNY')
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
    row = await connection.execute(
        text(_DETAIL_SELECT + " WHERE l.id = :listing_id AND l.creator_id = :creator_id"),
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
    predicate = "l.creator_id = :creator_id"
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
    media_type: str | None,
    min_price: Decimal | None,
    max_price: Decimal | None,
    sort: Literal["newest", "price_asc", "price_desc", "relevance"],
    after: dict[str, object] | None,
    limit: int,
) -> list[ListingRecord]:
    predicates = ["l.status = 'published'", "l.current_published_version_id IS NOT NULL"]
    params: dict[str, object] = {"limit": limit}
    if q:
        predicates.append(
            "(to_tsvector('simple', concat_ws(' ', l.title, l.description, cp.handle, "
            "COALESCE((SELECT string_agg(lt.tag, ' ') FROM listing_tags lt WHERE lt.listing_id = l.id), ''))) "
            "@@ websearch_to_tsquery('simple', :q) OR l.title % :q OR cp.handle % :q)"
        )
        params["q"] = q
    if tag:
        predicates.append("EXISTS (SELECT 1 FROM listing_tags lt WHERE lt.listing_id = l.id AND lt.tag = :tag)")
        params["tag"] = tag
    if creator:
        predicates.append("cp.handle ILIKE :creator")
        params["creator"] = f"%{creator}%"
    if media_type:
        predicates.append("a.media_type::text = :media_type")
        params["media_type"] = media_type
    if min_price is not None:
        predicates.append("l.price_amount >= :min_price")
        params["min_price"] = min_price
    if max_price is not None:
        predicates.append("l.price_amount <= :max_price")
        params["max_price"] = max_price

    rank = "ts_rank(to_tsvector('simple', concat_ws(' ', l.title, l.description, cp.handle)), websearch_to_tsquery('simple', :q))"
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
