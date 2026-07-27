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
    amount: Decimal
    expires_at: datetime


@dataclass(frozen=True, slots=True)
class LockedPaymentAttempt:
    id: UUID
    order_id: UUID
    buyer_id: UUID
    out_trade_no: str
    alipay_trade_no: str | None
    status: str
    amount: Decimal
    expires_at: datetime
    order_status: str
    order_amount: Decimal
    items: list[OrderItemRecord]


@dataclass(frozen=True, slots=True)
class ReversalRecord:
    id: UUID
    status: str
    amount: Decimal


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
                                       status="created", amount=offer.price, expires_at=expires_at)


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


async def find_attempt_for_reconciliation(
    connection: AsyncConnection, *, payment_attempt_id: UUID
) -> PaymentAttemptRecord | None:
    row = await connection.execute(
        text("""
            SELECT p.id, p.order_id, p.out_trade_no, p.status::text AS status, p.amount, p.expires_at
            FROM payment_attempts p JOIN orders o ON o.id = p.order_id
            WHERE p.id = :payment_attempt_id AND p.status IN ('created', 'pending', 'succeeded')
              AND o.status IN ('pending_payment', 'fulfilled')
        """), {"payment_attempt_id": payment_attempt_id},
    )
    value = row.mappings().one_or_none()
    if value is None:
        return None
    return PaymentAttemptRecord(id=value["id"], order_id=value["order_id"], out_trade_no=str(value["out_trade_no"]),
                                status=str(value["status"]), amount=Decimal(value["amount"]), expires_at=value["expires_at"])


async def lock_attempt_by_out_trade_no(
    connection: AsyncConnection, *, out_trade_no: str
) -> LockedPaymentAttempt | None:
    row = await connection.execute(
        text("""
            SELECT p.id, p.order_id, p.out_trade_no, p.alipay_trade_no, p.status::text AS status,
                   p.amount, p.expires_at, o.buyer_id, o.status::text AS order_status, o.amount AS order_amount
            FROM payment_attempts p JOIN orders o ON o.id = p.order_id
            WHERE p.out_trade_no = :out_trade_no
            FOR UPDATE OF p, o
        """), {"out_trade_no": out_trade_no},
    )
    value = row.mappings().one_or_none()
    if value is None:
        return None
    return await _locked_attempt(connection, dict(value))


async def lock_attempt_by_id(connection: AsyncConnection, *, payment_attempt_id: UUID) -> LockedPaymentAttempt | None:
    row = await connection.execute(
        text("""
            SELECT p.id, p.order_id, p.out_trade_no, p.alipay_trade_no, p.status::text AS status,
                   p.amount, p.expires_at, o.buyer_id, o.status::text AS order_status, o.amount AS order_amount
            FROM payment_attempts p JOIN orders o ON o.id = p.order_id
            WHERE p.id = :payment_attempt_id
            FOR UPDATE OF p, o
        """), {"payment_attempt_id": payment_attempt_id},
    )
    value = row.mappings().one_or_none()
    return await _locked_attempt(connection, dict(value)) if value is not None else None


async def _locked_attempt(connection: AsyncConnection, value: dict[str, Any]) -> LockedPaymentAttempt:
    items = [_item(item) for item in (await connection.execute(text(_ORDER_ITEMS), {"order_id": value["order_id"]})).mappings()]
    return LockedPaymentAttempt(
        id=value["id"], order_id=value["order_id"], buyer_id=value["buyer_id"], out_trade_no=str(value["out_trade_no"]),
        alipay_trade_no=str(value["alipay_trade_no"]) if value["alipay_trade_no"] else None,
        status=str(value["status"]), amount=Decimal(value["amount"]), expires_at=value["expires_at"],
        order_status=str(value["order_status"]), order_amount=Decimal(value["order_amount"]), items=items,
    )


async def insert_payment_notification(
    connection: AsyncConnection, *, payment_attempt_id: UUID, fingerprint: str, trade_status: str,
    payload_redacted: dict[str, object],
) -> bool:
    result = await connection.execute(
        text("""
            INSERT INTO payment_notifications (id, payment_attempt_id, fingerprint, trade_status, payload_redacted)
            VALUES (:id, :payment_attempt_id, :fingerprint, :trade_status, CAST(:payload_redacted AS jsonb))
            ON CONFLICT (fingerprint) DO NOTHING RETURNING id
        """), {"id": uuid4(), "payment_attempt_id": payment_attempt_id, "fingerprint": fingerprint,
             "trade_status": trade_status, "payload_redacted": json.dumps(payload_redacted)},
    )
    return result.scalar_one_or_none() is not None


async def mark_settled(
    connection: AsyncConnection, *, attempt: LockedPaymentAttempt, alipay_trade_no: str
) -> bool:
    if attempt.status == "succeeded" and attempt.order_status == "fulfilled":
        return False
    if attempt.status not in {"created", "pending"} or attempt.order_status != "pending_payment":
        return False
    await connection.execute(
        text("""
            UPDATE payment_attempts SET status = 'succeeded', alipay_trade_no = :alipay_trade_no
            WHERE id = :id
        """), {"id": attempt.id, "alipay_trade_no": alipay_trade_no},
    )
    await connection.execute(
        text("""
            UPDATE orders SET status = 'fulfilled', paid_at = COALESCE(paid_at, now()) WHERE id = :id
        """), {"id": attempt.order_id},
    )
    return True


async def grant_entitlements(connection: AsyncConnection, *, buyer_id: UUID, items: list[OrderItemRecord]) -> None:
    for item in items:
        await connection.execute(
            text("""
                INSERT INTO entitlements (id, user_id, asset_id, order_item_id, status)
                VALUES (:id, :user_id, :asset_id, :order_item_id, 'active')
                ON CONFLICT (order_item_id) DO NOTHING
            """), {"id": uuid4(), "user_id": buyer_id, "asset_id": item.asset_id, "order_item_id": item.id},
        )


async def close_unpaid(connection: AsyncConnection, *, attempt: LockedPaymentAttempt) -> bool:
    if attempt.status not in {"created", "pending"} or attempt.order_status != "pending_payment":
        return False
    await connection.execute(text("UPDATE payment_attempts SET status = 'closed' WHERE id = :id"), {"id": attempt.id})
    await connection.execute(text("UPDATE orders SET status = 'closed' WHERE id = :id"), {"id": attempt.order_id})
    return True


async def create_or_get_reversal(
    connection: AsyncConnection, *, attempt: LockedPaymentAttempt, amount: Decimal, external_reference: str
) -> ReversalRecord:
    inserted = await connection.execute(
        text("""
            INSERT INTO refund_reversals (id, order_id, payment_attempt_id, amount, currency, status, external_reference)
            VALUES (:id, :order_id, :payment_attempt_id, :amount, 'CNY', 'detected', :external_reference)
            ON CONFLICT (payment_attempt_id) DO NOTHING RETURNING id, status::text AS status, amount
        """), {"id": uuid4(), "order_id": attempt.order_id, "payment_attempt_id": attempt.id,
             "amount": amount, "external_reference": external_reference[:160]},
    )
    row = inserted.mappings().one_or_none()
    if row is None:
        found = await connection.execute(
            text("SELECT id, status::text AS status, amount FROM refund_reversals WHERE payment_attempt_id = :payment_attempt_id FOR UPDATE"),
            {"payment_attempt_id": attempt.id},
        )
        row = found.mappings().one()
    return ReversalRecord(id=row["id"], status=str(row["status"]), amount=Decimal(row["amount"]))


async def mark_reversal_manual_review(
    connection: AsyncConnection, *, reversal_id: UUID, order_id: UUID
) -> None:
    await connection.execute(
        text("UPDATE refund_reversals SET status = 'manual_review' WHERE id = :id AND status = 'detected'"),
        {"id": reversal_id},
    )
    await connection.execute(text("UPDATE orders SET status = 'payment_exception' WHERE id = :id"), {"id": order_id})


async def apply_reversal(connection: AsyncConnection, *, reversal_id: UUID, attempt: LockedPaymentAttempt) -> None:
    await connection.execute(
        text("UPDATE entitlements SET status = 'revoked', revoked_at = COALESCE(revoked_at, now()) WHERE order_item_id = ANY(CAST(:item_ids AS uuid[]))"),
        {"item_ids": [item.id for item in attempt.items]},
    )
    await connection.execute(
        text("UPDATE payment_attempts SET status = 'failed' WHERE id = :id"), {"id": attempt.id}
    )
    await connection.execute(text("UPDATE orders SET status = 'payment_exception' WHERE id = :id"), {"id": attempt.order_id})
    await connection.execute(
        text("UPDATE refund_reversals SET status = 'applied', applied_at = now() WHERE id = :id AND status = 'detected'"),
        {"id": reversal_id},
    )


async def mark_payment_exception(connection: AsyncConnection, *, payment_attempt_id: UUID) -> None:
    attempt = await lock_attempt_by_id(connection, payment_attempt_id=payment_attempt_id)
    if attempt is not None and attempt.order_status == "pending_payment":
        await connection.execute(text("UPDATE orders SET status = 'payment_exception' WHERE id = :id"), {"id": attempt.order_id})


async def list_pending_attempt_ids(connection: AsyncConnection, *, limit: int) -> list[UUID]:
    rows = await connection.execute(
        text("""
            SELECT p.id FROM payment_attempts p JOIN orders o ON o.id = p.order_id
            WHERE p.status IN ('created', 'pending') AND o.status = 'pending_payment'
            ORDER BY p.expires_at, p.id LIMIT :limit
        """), {"limit": limit},
    )
    return list(rows.scalars())
