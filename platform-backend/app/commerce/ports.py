"""Consumer-owned M6 ledger port used by M5 settlement/reversal code."""

from __future__ import annotations

from typing import Protocol
from uuid import UUID

from sqlalchemy.ext.asyncio import AsyncConnection


class SaleLedgerWriter(Protocol):
    """All calls use M5's already-open transaction; browser money never reaches this port."""

    async def record_sale(
        self, connection: AsyncConnection, *, order_item_id: UUID, request_id_value: str
    ) -> None: ...

    async def reverse_sale(
        self, connection: AsyncConnection, *, order_item_id: UUID, request_id_value: str
    ) -> None: ...
