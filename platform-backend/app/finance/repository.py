"""M6 journal persistence and transaction-bound creator balance projection."""

from __future__ import annotations

from datetime import datetime
from decimal import Decimal
from uuid import UUID, uuid4

from sqlalchemy import text
from sqlalchemy.ext.asyncio import AsyncConnection

from app.finance.models import CreatorBalance, FrozenOrderItem, LedgerEntry, PayoutRequestRecord


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


async def get_creator_balance(connection: AsyncConnection, *, creator_id: UUID) -> CreatorBalance:
    row = await connection.execute(
        text(
            """
            SELECT creator_id, available_amount, reserved_amount, currency
            FROM creator_balances WHERE creator_id = :creator_id
            """
        ),
        {"creator_id": creator_id},
    )
    value = row.mappings().one_or_none()
    if value is None:
        return CreatorBalance(
            creator_id=creator_id, available_amount=Decimal("0.00"),
            reserved_amount=Decimal("0.00"), currency="CNY",
        )
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


def _payout_record(row: object) -> PayoutRequestRecord:
    value = dict(row)  # type: ignore[arg-type]
    return PayoutRequestRecord(
        id=value["id"], creator_id=value["creator_id"], amount=Decimal(value["amount"]),
        currency=str(value["currency"]), qr_object_key=str(value["qr_object_key"]),
        status=str(value["status"]), external_reference=value["external_reference"],
        rejection_reason=value["rejection_reason"], operator_user_id=value["operator_user_id"],
        created_at=value["created_at"], paid_at=value["paid_at"], rejected_at=value["rejected_at"],
        cancelled_at=value["cancelled_at"], qr_deleted_at=value["qr_deleted_at"],
        creator_email=value.get("creator_email"), creator_handle=value.get("creator_handle"),
        operator_email=value.get("operator_email"),
    )


async def reserve_and_create_payout(
    connection: AsyncConnection, *, creator_id: UUID, amount: Decimal, qr_object_key: str
) -> PayoutRequestRecord | None:
    """Caller must lock creator balance before this compare-and-set reservation."""
    updated = await connection.execute(
        text("""UPDATE creator_balances
            SET available_amount = available_amount - :amount, reserved_amount = reserved_amount + :amount,
                updated_at = now()
            WHERE creator_id = :creator_id AND currency = 'CNY' AND available_amount >= :amount
            RETURNING creator_id"""),
        {"creator_id": creator_id, "amount": amount},
    )
    if updated.scalar_one_or_none() is None:
        return None
    result = await connection.execute(
        text("""INSERT INTO payout_requests (id, creator_id, amount, currency, qr_object_key, status)
            VALUES (:id, :creator_id, :amount, 'CNY', :qr_object_key, 'pending')
            RETURNING id, creator_id, amount, currency, qr_object_key, status, external_reference,
              rejection_reason, operator_user_id, created_at, paid_at, rejected_at, cancelled_at, qr_deleted_at"""),
        {"id": uuid4(), "creator_id": creator_id, "amount": amount, "qr_object_key": qr_object_key},
    )
    return _payout_record(result.mappings().one())


async def append_payout_entry(
    connection: AsyncConnection, *, payout_request_id: UUID, creator_id: UUID,
    account: str, signed_amount: Decimal, entry_type: str,
) -> LedgerEntry:
    entry_id = uuid4()
    await connection.execute(
        text("""INSERT INTO ledger_entries
            (id, creator_id, order_item_id, payout_request_id, account, signed_amount, currency, entry_type)
            VALUES (:id, :creator_id, NULL, :payout_request_id, CAST(:account AS ledger_account),
                    :signed_amount, 'CNY', CAST(:entry_type AS ledger_entry_type))"""),
        {"id": entry_id, "creator_id": creator_id, "payout_request_id": payout_request_id,
         "account": account, "signed_amount": signed_amount, "entry_type": entry_type},
    )
    return LedgerEntry(
        id=entry_id, creator_id=creator_id, order_item_id=None, payout_request_id=payout_request_id,
        account=account, signed_amount=signed_amount, currency="CNY", entry_type=entry_type,
    )


async def find_creator_payouts(
    connection: AsyncConnection, *, creator_id: UUID, limit: int,
    before_created_at: datetime | None = None, before_id: UUID | None = None,
) -> list[PayoutRequestRecord]:
    clause = ""
    params: dict[str, object] = {"creator_id": creator_id, "limit": limit}
    if before_created_at is not None and before_id is not None:
        clause = "AND (created_at, id) < (:before_created_at, :before_id)"
        params.update({"before_created_at": before_created_at, "before_id": before_id})
    rows = await connection.execute(text(f"""SELECT id, creator_id, amount, currency, qr_object_key,
        status::text AS status, external_reference, rejection_reason, operator_user_id, created_at, paid_at,
        rejected_at, cancelled_at, qr_deleted_at FROM payout_requests WHERE creator_id = :creator_id {clause}
        ORDER BY created_at DESC, id DESC LIMIT :limit"""), params)
    return [_payout_record(row) for row in rows.mappings()]


async def find_operator_payouts(
    connection: AsyncConnection, *, status: str | None, limit: int,
    before_created_at: datetime | None = None, before_id: UUID | None = None,
) -> list[PayoutRequestRecord]:
    predicates = ["TRUE"]
    params: dict[str, object] = {"limit": limit}
    if status is not None:
        predicates.append("p.status::text = :status")
        params["status"] = status
    if before_created_at is not None and before_id is not None:
        predicates.append("(p.created_at, p.id) < (:before_created_at, :before_id)")
        params.update({"before_created_at": before_created_at, "before_id": before_id})
    rows = await connection.execute(text(f"""SELECT p.id, p.creator_id, p.amount, p.currency, p.qr_object_key,
        p.status::text AS status, p.external_reference, p.rejection_reason, p.operator_user_id, p.created_at,
        p.paid_at, p.rejected_at, p.cancelled_at, p.qr_deleted_at, creator.email AS creator_email,
        profile.handle AS creator_handle, operator.email AS operator_email
        FROM payout_requests p JOIN users creator ON creator.id = p.creator_id
        LEFT JOIN creator_profiles profile ON profile.user_id = p.creator_id
        LEFT JOIN users operator ON operator.id = p.operator_user_id
        WHERE {' AND '.join(predicates)} ORDER BY p.created_at DESC, p.id DESC LIMIT :limit"""), params)
    return [_payout_record(row) for row in rows.mappings()]


async def lock_payout_request(connection: AsyncConnection, *, payout_request_id: UUID) -> PayoutRequestRecord | None:
    result = await connection.execute(text("""SELECT id, creator_id, amount, currency, qr_object_key,
        status::text AS status, external_reference, rejection_reason, operator_user_id, created_at, paid_at,
        rejected_at, cancelled_at, qr_deleted_at FROM payout_requests WHERE id = :id FOR UPDATE"""), {"id": payout_request_id})
    value = result.mappings().one_or_none()
    return _payout_record(value) if value is not None else None


async def cancel_payout(connection: AsyncConnection, *, payout_request_id: UUID, creator_id: UUID) -> PayoutRequestRecord | None:
    result = await connection.execute(text("""UPDATE payout_requests SET status = 'cancelled', cancelled_at = now()
        WHERE id = :id AND creator_id = :creator_id AND status = 'pending'
        RETURNING id, creator_id, amount, currency, qr_object_key, status::text AS status, external_reference,
          rejection_reason, operator_user_id, created_at, paid_at, rejected_at, cancelled_at, qr_deleted_at"""),
        {"id": payout_request_id, "creator_id": creator_id})
    value = result.mappings().one_or_none()
    return _payout_record(value) if value is not None else None


async def settle_payout(connection: AsyncConnection, *, payout_request_id: UUID, operator_user_id: UUID,
    next_status: str, external_reference: str | None = None, rejection_reason: str | None = None) -> PayoutRequestRecord | None:
    result = await connection.execute(text("""UPDATE payout_requests
        SET status = CAST(:next_status AS payout_request_status), operator_user_id = :operator_user_id,
            external_reference = :external_reference, rejection_reason = :rejection_reason,
            paid_at = CASE WHEN :next_status = 'paid' THEN now() ELSE NULL END,
            rejected_at = CASE WHEN :next_status = 'rejected' THEN now() ELSE NULL END
        WHERE id = :id AND status = 'pending'
        RETURNING id, creator_id, amount, currency, qr_object_key, status::text AS status, external_reference,
          rejection_reason, operator_user_id, created_at, paid_at, rejected_at, cancelled_at, qr_deleted_at"""),
        {"id": payout_request_id, "operator_user_id": operator_user_id, "next_status": next_status,
         "external_reference": external_reference, "rejection_reason": rejection_reason})
    value = result.mappings().one_or_none()
    return _payout_record(value) if value is not None else None


async def release_or_consume_reservation(connection: AsyncConnection, *, creator_id: UUID, amount: Decimal,
    release_to_available: bool) -> CreatorBalance | None:
    extra = "available_amount = available_amount + :amount," if release_to_available else ""
    result = await connection.execute(text(f"""UPDATE creator_balances SET {extra}
        reserved_amount = reserved_amount - :amount, updated_at = now()
        WHERE creator_id = :creator_id AND currency = 'CNY' AND reserved_amount >= :amount
        RETURNING creator_id, available_amount, reserved_amount, currency"""),
        {"creator_id": creator_id, "amount": amount})
    value = result.mappings().one_or_none()
    if value is None:
        return None
    return CreatorBalance(creator_id=value["creator_id"], available_amount=Decimal(value["available_amount"]),
        reserved_amount=Decimal(value["reserved_amount"]), currency=str(value["currency"]))


async def mark_qr_deleted(connection: AsyncConnection, *, payout_request_id: UUID) -> str | None:
    result = await connection.execute(text("""UPDATE payout_requests
        SET qr_deleted_at = COALESCE(qr_deleted_at, now())
        WHERE id = :id AND status IN ('paid', 'rejected', 'cancelled')
        RETURNING qr_object_key"""), {"id": payout_request_id})
    return result.scalar_one_or_none()
