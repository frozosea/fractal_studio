"""M6 journal persistence and transaction-bound creator balance projection."""

from __future__ import annotations

from decimal import Decimal
from uuid import UUID, uuid4

from sqlalchemy import text
from sqlalchemy.ext.asyncio import AsyncConnection

from app.finance.models import CreatorBalance, FrozenOrderItem, LedgerEntry


async def lock_frozen_order_item(
    connection: AsyncConnection, *, order_item_id: UUID
) -> FrozenOrderItem | None:
    row = await connection.execute(
        text(
            """
            SELECT id, creator_id, price_amount, creator_amount, platform_fee_amount, currency
            FROM order_items WHERE id = :order_item_id FOR UPDATE
            """
        ),
        {"order_item_id": order_item_id},
    )
    value = row.mappings().one_or_none()
    if value is None:
        return None
    return FrozenOrderItem(
        id=value["id"], creator_id=value["creator_id"], price_amount=Decimal(value["price_amount"]),
        creator_amount=Decimal(value["creator_amount"]),
        platform_fee_amount=Decimal(value["platform_fee_amount"]), currency=str(value["currency"]),
    )


async def existing_order_entry_types(connection: AsyncConnection, *, order_item_id: UUID) -> set[str]:
    rows = await connection.execute(
        text("SELECT entry_type::text FROM ledger_entries WHERE order_item_id = :order_item_id"),
        {"order_item_id": order_item_id},
    )
    return {str(value) for value in rows.scalars()}


async def append_order_entry(
    connection: AsyncConnection,
    *,
    creator_id: UUID | None,
    order_item_id: UUID,
    account: str,
    signed_amount: Decimal,
    entry_type: str,
    currency: str = "CNY",
) -> LedgerEntry:
    entry_id = uuid4()
    await connection.execute(
        text(
            """
            INSERT INTO ledger_entries
                (id, creator_id, order_item_id, payout_request_id, account, signed_amount, currency, entry_type)
            VALUES
                (:id, :creator_id, :order_item_id, NULL, CAST(:account AS ledger_account), :signed_amount,
                 :currency, CAST(:entry_type AS ledger_entry_type))
            """
        ),
        {
            "id": entry_id, "creator_id": creator_id, "order_item_id": order_item_id, "account": account,
            "signed_amount": signed_amount, "currency": currency, "entry_type": entry_type,
        },
    )
    return LedgerEntry(
        id=entry_id, creator_id=creator_id, order_item_id=order_item_id, payout_request_id=None,
        account=account, signed_amount=signed_amount, currency=currency, entry_type=entry_type,
    )


async def lock_creator_balance(connection: AsyncConnection, *, creator_id: UUID) -> CreatorBalance:
    """Create zero projection then lock it; serializes sale/reversal changes per creator."""
    await connection.execute(
        text(
            """
            INSERT INTO creator_balances (creator_id, available_amount, reserved_amount, currency)
            VALUES (:creator_id, 0, 0, 'CNY') ON CONFLICT (creator_id) DO NOTHING
            """
        ),
        {"creator_id": creator_id},
    )
    row = await connection.execute(
        text(
            """
            SELECT creator_id, available_amount, reserved_amount, currency
            FROM creator_balances WHERE creator_id = :creator_id FOR UPDATE
            """
        ),
        {"creator_id": creator_id},
    )
    value = row.mappings().one()
    return CreatorBalance(
        creator_id=value["creator_id"], available_amount=Decimal(value["available_amount"]),
        reserved_amount=Decimal(value["reserved_amount"]), currency=str(value["currency"]),
    )


async def apply_available_delta(
    connection: AsyncConnection, *, creator_id: UUID, delta: Decimal, require_nonnegative: bool
) -> CreatorBalance | None:
    predicate = "AND available_amount + :delta >= 0" if require_nonnegative else ""
    updated = await connection.execute(
        text(
            f"""
            UPDATE creator_balances SET available_amount = available_amount + :delta, updated_at = now()
            WHERE creator_id = :creator_id {predicate}
            RETURNING creator_id, available_amount, reserved_amount, currency
            """
        ),
        {"creator_id": creator_id, "delta": delta},
    )
    value = updated.mappings().one_or_none()
    if value is None:
        return None
    return CreatorBalance(
        creator_id=value["creator_id"], available_amount=Decimal(value["available_amount"]),
        reserved_amount=Decimal(value["reserved_amount"]), currency=str(value["currency"]),
    )


async def list_order_entries(connection: AsyncConnection, *, order_item_id: UUID) -> list[LedgerEntry]:
    rows = await connection.execute(
        text(
            """
            SELECT id, creator_id, order_item_id, payout_request_id, account::text AS account,
                   signed_amount, currency, entry_type::text AS entry_type
            FROM ledger_entries WHERE order_item_id = :order_item_id ORDER BY created_at, id
            """
        ),
        {"order_item_id": order_item_id},
    )
    return [
        LedgerEntry(
            id=row["id"], creator_id=row["creator_id"], order_item_id=row["order_item_id"],
            payout_request_id=row["payout_request_id"], account=str(row["account"]),
            signed_amount=Decimal(row["signed_amount"]), currency=str(row["currency"]),
            entry_type=str(row["entry_type"]),
        )
        for row in rows.mappings()
    ]
