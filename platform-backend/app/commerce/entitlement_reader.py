"""M5-owned read adapter consumed by M3 without exposing commerce tables."""

from __future__ import annotations

from uuid import UUID

from sqlalchemy import text

from app.core.db import get_engine


class PostgresEntitlementReader:
    """Answers only active entitlement existence; order/payment data stays private to M5."""

    async def has_active_entitlement(self, *, user_id: UUID, asset_id: UUID) -> bool:
        async with get_engine().connect() as connection:
            return bool(
                await connection.scalar(
                    text(
                        """
                        SELECT EXISTS (
                          SELECT 1 FROM entitlements
                          WHERE buyer_id = :user_id AND asset_id = :asset_id AND status = 'active'
                        )
                        """
                    ),
                    {"user_id": user_id, "asset_id": asset_id},
                )
            )
