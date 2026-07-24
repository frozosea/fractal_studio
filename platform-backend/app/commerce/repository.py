"""M5 PostgreSQL checkout persistence; all writes use caller transaction."""

from __future__ import annotations

import json
from dataclasses import dataclass
from datetime import datetime
from decimal import Decimal
from typing import Any
from uuid import UUID, uuid4

from sqlalchemy import text
from sqlalchemy.ext.asyncio import AsyncConnection

from app.marketplace.ports import PublishedOfferSnapshot


@dataclass(frozen=True, slots=True)
class OrderItemRecord:
    id: UUID
    listing_id: UUID
    listing_version_id: UUID
    licence_offer_id: UUID
    asset_id: UUID
    creator_id: UUID
    price: Decimal
    currency: str
    commission_policy_version: str
    creator_amount: Decimal
    platform_fee_amount: Decimal


@dataclass(frozen=True, slots=True)
class OrderRecord:
    id: UUID
    buyer_id: UUID
    status: str
    amount: Decimal
    currency: str
    paid_at: datetime | None
    created_at: datetime
    items: list[OrderItemRecord]


@dataclass(frozen=True, slots=True)
class PaymentAttemptRecord:
    id: UUID
    order_id: UUID
    out_trade_no: str
    status: str
    expires_at: datetime


def _item(row: Any) -> OrderItemRecord:
    data = dict(row)
    return OrderItemRecord(
        id=data["item_id"], listing_id=data["listing_id"], listing_version_id=data["listing_version_id"],
        licence_offer_id=data["licence_offer_id"], asset_id=data["asset_id"], creator_id=data["creator_id"],
        price=Decimal(data["price_amount"]), currency=str(data["item_currency"]),
        commission_policy_version=str(data["commission_policy_version"]), creator_amount=Decimal(data["creator_amount"]),
        platform_fee_amount=Decimal(data["platform_fee_amount"]),
    )


_ORDER_ITEMS = """
    SELECT oi.id AS item_id, oi.listing_id, oi.listing_version_id, oi.licence_offer_id, oi.asset_id, oi.creator_id,
           oi.price_amount, oi.currency AS item_currency, oi.commission_policy_version,
           oi.creator_amount, oi.platform_fee_amount
    FROM order_items oi WHERE oi.order_id = :order_id ORDER BY oi.id
"""


async def create_pending_order(
    connection: AsyncConnection,
    *,
    buyer_id: UUID,
    offer: PublishedOfferSnapshot,
    commission_policy_version: str,
    creator_amount: Decimal,
    platform_fee_amount: Decimal,
    out_trade_no: str,
    expires_at: datetime,
) -> tuple[OrderRecord, PaymentAttemptRecord]:
    order_id, item_id, attempt_id = uuid4(), uuid4(), uuid4()
    await connection.execute(
        text("""
            INSERT INTO orders (id, buyer_id, status, amount, currency)
            VALUES (:id, :buyer_id, 'pending_payment', :amount, 'CNY')
        """),
        {"id": order_id, "buyer_id": buyer_id, "amount": offer.price},
    )
    await connection.execute(
        text("""
            INSERT INTO order_items (
                id, order_id, listing_id, listing_version_id, licence_offer_id, asset_id, creator_id,
                price_amount, commission_policy_version, creator_amount, platform_fee_amount, currency,
                listing_snapshot_json, licence_snapshot_json
            ) VALUES (
                :id, :order_id, :listing_id, :listing_version_id, :licence_offer_id, :asset_id, :creator_id,
                :price, :commission_policy_version, :creator_amount, :platform_fee_amount, 'CNY',
                CAST(:listing_snapshot AS jsonb), CAST(:licence_snapshot AS jsonb)
            )
        """),
        {"id": item_id, "order_id": order_id, "listing_id": offer.listing_id,
         "listing_version_id": offer.listing_version_id, "licence_offer_id": offer.licence_offer_id,
         "asset_id": offer.asset_id, "creator_id": offer.creator_id, "price": offer.price,
         "commission_policy_version": commission_policy_version, "creator_amount": creator_amount,
         "platform_fee_amount": platform_fee_amount, "listing_snapshot": json.dumps(offer.listing_snapshot),
         "licence_snapshot": json.dumps(offer.licence_snapshot)},
    )
    await connection.execute(
        text("""
            INSERT INTO payment_attempts (id, order_id, out_trade_no, status, amount, expires_at)
            VALUES (:id, :order_id, :out_trade_no, 'created', :amount, :expires_at)
        """),
        {"id": attempt_id, "order_id": order_id, "out_trade_no": out_trade_no,
         "amount": offer.price, "expires_at": expires_at},
    )
    order = await find_order(connection, order_id=order_id, buyer_id=buyer_id)
    assert order is not None
    return order, PaymentAttemptRecord(id=attempt_id, order_id=order_id, out_trade_no=out_trade_no,
                                       status="created", expires_at=expires_at)


async def find_active_order_for_listing(
    connection: AsyncConnection, *, buyer_id: UUID, listing_id: UUID
) -> UUID | None:
    return await connection.scalar(
        text("""
            SELECT o.id
            FROM orders o JOIN order_items oi ON oi.order_id = o.id
            WHERE o.buyer_id = :buyer_id AND oi.listing_id = :listing_id AND o.status = 'pending_payment'
            ORDER BY o.created_at DESC LIMIT 1
        """), {"buyer_id": buyer_id, "listing_id": listing_id},
    )


async def find_order(connection: AsyncConnection, *, order_id: UUID, buyer_id: UUID) -> OrderRecord | None:
    result = await connection.execute(
        text("""
            SELECT id, buyer_id, status::text AS status, amount, currency, paid_at, created_at
            FROM orders WHERE id = :order_id AND buyer_id = :buyer_id
        """), {"order_id": order_id, "buyer_id": buyer_id},
    )
    row = result.mappings().one_or_none()
    if row is None:
        return None
    data = dict(row)
    items = [_item(item) for item in (await connection.execute(text(_ORDER_ITEMS), {"order_id": order_id})).mappings()]
    return OrderRecord(id=data["id"], buyer_id=data["buyer_id"], status=str(data["status"]), amount=Decimal(data["amount"]),
                       currency=str(data["currency"]), paid_at=data["paid_at"], created_at=data["created_at"], items=items)


async def list_buyer_orders(
    connection: AsyncConnection, *, buyer_id: UUID, limit: int, before: tuple[datetime, UUID] | None
) -> list[OrderRecord]:
    params: dict[str, object] = {"buyer_id": buyer_id, "limit": limit}
    predicate = "buyer_id = :buyer_id"
    if before is not None:
        predicate += " AND (created_at, id) < (:before_created_at, :before_id)"
        params.update({"before_created_at": before[0], "before_id": before[1]})
    rows = (await connection.execute(text(f"""
        SELECT id, buyer_id, status::text AS status, amount, currency, paid_at, created_at
        FROM orders WHERE {predicate} ORDER BY created_at DESC, id DESC LIMIT :limit
    """), params)).mappings()
    orders: list[OrderRecord] = []
    for row in rows:
        data = dict(row)
        order_id = data["id"]
        items = [_item(item) for item in (await connection.execute(text(_ORDER_ITEMS), {"order_id": order_id})).mappings()]
        orders.append(OrderRecord(id=order_id, buyer_id=data["buyer_id"], status=str(data["status"]),
            amount=Decimal(data["amount"]), currency=str(data["currency"]), paid_at=data["paid_at"],
            created_at=data["created_at"], items=items))
    return orders
