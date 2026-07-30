"""SQL persistence for administrator views and mutations."""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime
from decimal import Decimal
from typing import Any
from uuid import UUID

from sqlalchemy import text
from sqlalchemy.ext.asyncio import AsyncConnection


@dataclass(frozen=True, slots=True)
class AdminUserRecord:
    id: UUID
    email: str
    status: str
    roles: list[str]
    member: bool
    creator_handle: str | None
    creator_display_name: str | None
    asset_count: int
    listing_count: int
    fulfilled_order_count: int
    created_at: datetime


@dataclass(frozen=True, slots=True)
class AdminListingRecord:
    id: UUID
    asset_id: UUID
    creator_id: UUID
    creator_email: str
    creator_handle: str | None
    creator_display_name: str | None
    status: str
    title: str
    description: str
    tags: list[str]
    price: Decimal
    currency: str
    media_type: str
    favorite_count: int
    sale_count: int
    created_at: datetime
    published_at: datetime | None


def _user_record(row: Any) -> AdminUserRecord:
    data = dict(row)
    return AdminUserRecord(
        id=data["id"],
        email=str(data["email"]),
        status=str(data["status"]),
        roles=list(data["roles"] or []),
        member=bool(data["member"]),
        creator_handle=str(data["creator_handle"]) if data["creator_handle"] is not None else None,
        creator_display_name=(
            str(data["creator_display_name"])
            if data["creator_display_name"] is not None
            else None
        ),
        asset_count=int(data["asset_count"]),
        listing_count=int(data["listing_count"]),
        fulfilled_order_count=int(data["fulfilled_order_count"]),
        created_at=data["created_at"],
    )


def _listing_record(row: Any) -> AdminListingRecord:
    data = dict(row)
    return AdminListingRecord(
        id=data["id"],
        asset_id=data["asset_id"],
        creator_id=data["creator_id"],
        creator_email=str(data["creator_email"]),
        creator_handle=str(data["creator_handle"]) if data["creator_handle"] is not None else None,
        creator_display_name=(
            str(data["creator_display_name"])
            if data["creator_display_name"] is not None
            else None
        ),
        status=str(data["status"]),
        title=str(data["title"]),
        description=str(data["description"]),
        tags=list(data["tags"] or []),
        price=Decimal(data["price_amount"]),
        currency=str(data["currency"]),
        media_type=str(data["media_type"]),
        favorite_count=int(data["favorite_count"]),
        sale_count=int(data["sale_count"]),
        created_at=data["created_at"],
        published_at=data["published_at"],
    )


_USER_SELECT = """
    SELECT u.id, u.email, u.status::text AS status, u.created_at,
           COALESCE((SELECT array_agg(ur.role::text ORDER BY ur.role::text)
                     FROM user_roles ur WHERE ur.user_id = u.id), ARRAY[]::text[]) AS roles,
           EXISTS(SELECT 1 FROM memberships m
                  WHERE m.user_id = u.id AND m.status = 'active') AS member,
           cp.handle AS creator_handle, cp.display_name AS creator_display_name,
           (SELECT count(*) FROM assets a
            WHERE a.owner_id = u.id AND a.status <> 'deleted') AS asset_count,
           (SELECT count(*) FROM listings l
            WHERE l.creator_id = u.id AND l.status <> 'archived') AS listing_count,
           (SELECT count(*) FROM orders o
            WHERE o.buyer_id = u.id AND o.status = 'fulfilled') AS fulfilled_order_count
    FROM users u
    LEFT JOIN creator_profiles cp ON cp.user_id = u.id
"""


_LISTING_SELECT = """
    SELECT l.id, l.asset_id, l.creator_id, u.email AS creator_email,
           cp.handle AS creator_handle, cp.display_name AS creator_display_name,
           l.status::text AS status, l.title, l.description, l.price_amount,
           l.currency, l.created_at, l.published_at, a.media_type::text AS media_type,
           COALESCE((SELECT array_agg(lt.tag ORDER BY lt.tag)
                     FROM listing_tags lt WHERE lt.listing_id = l.id), ARRAY[]::text[]) AS tags,
           (SELECT count(*) FROM favorites f WHERE f.asset_id = l.asset_id) AS favorite_count,
           (SELECT count(*) FROM order_items oi JOIN orders o ON o.id = oi.order_id
            WHERE oi.listing_id = l.id AND o.status = 'fulfilled') AS sale_count
    FROM listings l
    JOIN users u ON u.id = l.creator_id
    LEFT JOIN creator_profiles cp ON cp.user_id = l.creator_id
    JOIN assets a ON a.id = l.asset_id
"""


async def statistics(connection: AsyncConnection) -> dict[str, object]:
    row = await connection.execute(
        text(
            """
            SELECT now() AS generated_at,
              (SELECT count(*) FROM users) AS users_total,
              (SELECT count(*) FROM users WHERE status = 'active') AS users_active,
              (SELECT count(*) FROM users WHERE status = 'disabled') AS users_disabled,
              (SELECT count(DISTINCT user_id) FROM user_roles WHERE role = 'creator') AS creators,
              (SELECT count(*) FROM memberships WHERE status = 'active') AS members,
              (SELECT count(DISTINCT user_id) FROM user_roles WHERE role = 'admin') AS admins,
              (SELECT count(*) FROM users WHERE created_at >= now() - interval '30 days') AS users_new_30d,
              (SELECT count(*) FROM listings) AS listings_total,
              (SELECT count(*) FROM listings WHERE status = 'published') AS listings_published,
              (SELECT count(*) FROM listings WHERE status = 'draft') AS listings_draft,
              (SELECT count(*) FROM listings WHERE status = 'unpublished') AS listings_unpublished,
              (SELECT count(*) FROM listings WHERE status = 'archived') AS listings_archived,
              (SELECT count(*) FROM assets WHERE status = 'ready') AS ready_assets,
              (SELECT count(*) FROM favorites) AS favorites,
              (SELECT count(*) FROM orders) AS orders_total,
              (SELECT count(*) FROM orders WHERE status = 'fulfilled') AS orders_fulfilled,
              (SELECT count(*) FROM orders WHERE status = 'pending_payment') AS orders_pending,
              (SELECT count(*) FROM orders WHERE status = 'payment_exception') AS orders_exception,
              (SELECT COALESCE(sum(oi.price_amount), 0) FROM order_items oi
               JOIN orders o ON o.id = oi.order_id WHERE o.status = 'fulfilled') AS marketplace_gross,
              (SELECT COALESCE(sum(o.amount), 0) FROM membership_orders mo
               JOIN orders o ON o.id = mo.order_id WHERE o.status = 'fulfilled') AS membership_revenue,
              (SELECT COALESCE(sum(oi.creator_amount), 0) FROM order_items oi
               JOIN orders o ON o.id = oi.order_id WHERE o.status = 'fulfilled') AS creator_revenue,
              (SELECT COALESCE(sum(oi.platform_fee_amount), 0) FROM order_items oi
               JOIN orders o ON o.id = oi.order_id WHERE o.status = 'fulfilled') AS platform_revenue,
              (SELECT count(*) FROM render_jobs) AS render_jobs_total,
              (SELECT count(*) FROM render_jobs WHERE status IN
                ('queued', 'submitting', 'running', 'compute_succeeded', 'ingesting', 'cancel_requested')) AS render_jobs_active,
              (SELECT count(*) FROM render_jobs WHERE status = 'completed') AS render_jobs_completed,
              (SELECT count(*) FROM render_jobs WHERE status IN ('failed', 'cancelled')) AS render_jobs_failed
            """
        )
    )
    return dict(row.mappings().one())


async def list_users(
    connection: AsyncConnection,
    *,
    q: str | None,
    user_status: str | None,
    role: str | None,
    before: tuple[datetime, UUID] | None,
    limit: int,
) -> list[AdminUserRecord]:
    predicates: list[str] = []
    params: dict[str, object] = {"limit": limit}
    if q:
        predicates.append("(u.email ILIKE :q OR cp.handle ILIKE :q OR cp.display_name ILIKE :q)")
        params["q"] = f"%{q}%"
    if user_status:
        predicates.append("u.status::text = :status")
        params["status"] = user_status
    if role:
        predicates.append(
            "EXISTS(SELECT 1 FROM user_roles filter_role "
            "WHERE filter_role.user_id = u.id AND filter_role.role::text = :role)"
        )
        params["role"] = role
    if before:
        predicates.append("(u.created_at, u.id) < (:before_at, :before_id)")
        params.update({"before_at": before[0], "before_id": before[1]})
    where = " WHERE " + " AND ".join(predicates) if predicates else ""
    rows = await connection.execute(
        text(_USER_SELECT + where + " ORDER BY u.created_at DESC, u.id DESC LIMIT :limit"),
        params,
    )
    return [_user_record(row) for row in rows.mappings()]


async def find_user(connection: AsyncConnection, user_id: UUID) -> AdminUserRecord | None:
    row = await connection.execute(
        text(_USER_SELECT + " WHERE u.id = :user_id"), {"user_id": user_id}
    )
    found = row.mappings().one_or_none()
    return _user_record(found) if found is not None else None


async def lock_user(connection: AsyncConnection, user_id: UUID) -> dict[str, object] | None:
    row = await connection.execute(
        text("SELECT id, status::text AS status FROM users WHERE id = :user_id FOR UPDATE"),
        {"user_id": user_id},
    )
    return row.mappings().one_or_none()


async def admin_count(connection: AsyncConnection) -> int:
    value = await connection.scalar(
        text("SELECT count(DISTINCT user_id) FROM user_roles WHERE role = 'admin'")
    )
    return int(value or 0)


async def update_user(
    connection: AsyncConnection,
    *,
    user_id: UUID,
    user_status: str,
    member: bool,
    privileged_roles: set[str],
    membership_granted_by: UUID,
) -> None:
    await connection.execute(
        text("UPDATE users SET status = CAST(:status AS user_status) WHERE id = :user_id"),
        {"status": user_status, "user_id": user_id},
    )
    await connection.execute(
        text(
            "DELETE FROM user_roles WHERE user_id = :user_id "
            "AND role::text IN ('admin', 'finance_operator')"
        ),
        {"user_id": user_id},
    )
    for role in sorted(privileged_roles):
        await connection.execute(
            text(
                "INSERT INTO user_roles (user_id, role) VALUES (:user_id, CAST(:role AS user_role)) "
                "ON CONFLICT (user_id, role) DO NOTHING"
            ),
            {"user_id": user_id, "role": role},
        )
    await connection.execute(
        text(
            "INSERT INTO memberships (user_id, status, granted_by) "
            "VALUES (:user_id, :membership_status, :granted_by) "
            "ON CONFLICT (user_id) DO UPDATE SET status = EXCLUDED.status, "
            "granted_by = EXCLUDED.granted_by, granted_at = now()"
        ),
        {
            "user_id": user_id,
            "membership_status": "active" if member else "revoked",
            "granted_by": membership_granted_by,
        },
    )
    if user_status == "disabled":
        await connection.execute(
            text(
                "UPDATE sessions SET revoked_at = now() "
                "WHERE user_id = :user_id AND revoked_at IS NULL"
            ),
            {"user_id": user_id},
        )


async def list_listings(
    connection: AsyncConnection,
    *,
    q: str | None,
    listing_status: str | None,
    before: tuple[datetime, UUID] | None,
    limit: int,
) -> list[AdminListingRecord]:
    predicates: list[str] = []
    params: dict[str, object] = {"limit": limit}
    if q:
        predicates.append(
            "(l.title ILIKE :q OR l.description ILIKE :q OR u.email ILIKE :q "
            "OR cp.handle ILIKE :q OR cp.display_name ILIKE :q)"
        )
        params["q"] = f"%{q}%"
    if listing_status:
        predicates.append("l.status::text = :status")
        params["status"] = listing_status
    if before:
        predicates.append("(l.created_at, l.id) < (:before_at, :before_id)")
        params.update({"before_at": before[0], "before_id": before[1]})
    where = " WHERE " + " AND ".join(predicates) if predicates else ""
    rows = await connection.execute(
        text(_LISTING_SELECT + where + " ORDER BY l.created_at DESC, l.id DESC LIMIT :limit"),
        params,
    )
    return [_listing_record(row) for row in rows.mappings()]


async def find_listing(
    connection: AsyncConnection, listing_id: UUID
) -> AdminListingRecord | None:
    row = await connection.execute(
        text(_LISTING_SELECT + " WHERE l.id = :listing_id"), {"listing_id": listing_id}
    )
    found = row.mappings().one_or_none()
    return _listing_record(found) if found is not None else None


async def lock_listing(connection: AsyncConnection, listing_id: UUID) -> dict[str, object] | None:
    row = await connection.execute(
        text("SELECT id, status::text AS status FROM listings WHERE id = :listing_id FOR UPDATE"),
        {"listing_id": listing_id},
    )
    return row.mappings().one_or_none()


async def moderate_listing(
    connection: AsyncConnection, *, listing_id: UUID, action: str
) -> None:
    next_status = "unpublished" if action == "unpublish" else "archived"
    await connection.execute(
        text(
            "UPDATE listings SET status = CAST(:status AS listing_status), "
            "published_at = NULL, current_published_version_id = NULL WHERE id = :listing_id"
        ),
        {"status": next_status, "listing_id": listing_id},
    )
