"""M6a immutable sale/reversal journal writer; no payout transfer or public route."""

from __future__ import annotations

from decimal import Decimal
from uuid import UUID

from sqlalchemy.ext.asyncio import AsyncConnection

from app.core import audit_writer
from app.finance import repository
from app.finance.models import InsufficientCreatorBalanceError, LedgerInvariantError


_CENT = Decimal("0.01")
_SALE_TYPES = {"creator_credit", "platform_fee"}
_REVERSAL_TYPES = {"creator_reversal", "platform_reversal"}


class PostgresSaleLedgerWriter:
    """Called by M5 in its settlement/reversal transaction, never by an HTTP finance endpoint."""

    async def record_sale(
        self, connection: AsyncConnection, *, order_item_id: UUID, request_id_value: str
    ) -> None:
        item = await self._locked_valid_item(connection, order_item_id=order_item_id)
        existing = await repository.existing_order_entry_types(connection, order_item_id=order_item_id)
        if _SALE_TYPES.issubset(existing):
            return
        if existing & _SALE_TYPES:
            raise LedgerInvariantError("partial_sale_journal")
        if existing & _REVERSAL_TYPES:
            raise LedgerInvariantError("reversal_without_sale")
        await repository.lock_creator_balance(connection, creator_id=item.creator_id)
        await repository.append_order_entry(
            connection, creator_id=item.creator_id, order_item_id=item.id, account="creator_available",
            signed_amount=item.creator_amount, entry_type="creator_credit",
        )
        await repository.append_order_entry(
            connection, creator_id=None, order_item_id=item.id, account="platform_revenue",
            signed_amount=item.platform_fee_amount, entry_type="platform_fee",
        )
        projection = await repository.apply_available_delta(
            connection, creator_id=item.creator_id, delta=item.creator_amount, require_nonnegative=True
        )
        assert projection is not None
        await audit_writer.record_system_action(
            connection, action="ledger.sale_recorded", subject_type="order_item", subject_id=item.id,
            request_id_value=request_id_value,
            metadata={"creatorId": str(item.creator_id), "currency": item.currency},
        )

    async def reverse_sale(
        self, connection: AsyncConnection, *, order_item_id: UUID, request_id_value: str
    ) -> None:
        item = await self._locked_valid_item(connection, order_item_id=order_item_id)
        existing = await repository.existing_order_entry_types(connection, order_item_id=order_item_id)
        if _REVERSAL_TYPES.issubset(existing):
            return
        if existing & _REVERSAL_TYPES:
            raise LedgerInvariantError("partial_reversal_journal")
        if not _SALE_TYPES.issubset(existing):
            raise LedgerInvariantError("sale_journal_missing")
        await repository.lock_creator_balance(connection, creator_id=item.creator_id)
        projection = await repository.apply_available_delta(
            connection, creator_id=item.creator_id, delta=-item.creator_amount, require_nonnegative=True
        )
        if projection is None:
            raise InsufficientCreatorBalanceError("creator_balance_already_reserved_or_paid")
        await repository.append_order_entry(
            connection, creator_id=item.creator_id, order_item_id=item.id, account="creator_available",
            signed_amount=-item.creator_amount, entry_type="creator_reversal",
        )
        await repository.append_order_entry(
            connection, creator_id=None, order_item_id=item.id, account="platform_revenue",
            signed_amount=-item.platform_fee_amount, entry_type="platform_reversal",
        )
        await audit_writer.record_system_action(
            connection, action="ledger.sale_reversed", subject_type="order_item", subject_id=item.id,
            request_id_value=request_id_value,
            metadata={"creatorId": str(item.creator_id), "currency": item.currency},
        )

    @staticmethod
    async def _locked_valid_item(connection: AsyncConnection, *, order_item_id: UUID):
        item = await repository.lock_frozen_order_item(connection, order_item_id=order_item_id)
        if item is None:
            raise LedgerInvariantError("order_item_not_found")
        amounts = (item.price_amount, item.creator_amount, item.platform_fee_amount)
        if (
            item.currency != "CNY"
            or any(not amount.is_finite() or amount.as_tuple().exponent < -2 for amount in amounts)
            or item.price_amount < _CENT
            or item.creator_amount < 0
            or item.platform_fee_amount < 0
            or item.creator_amount + item.platform_fee_amount != item.price_amount
        ):
            raise LedgerInvariantError("invalid_frozen_order_split")
        return item
