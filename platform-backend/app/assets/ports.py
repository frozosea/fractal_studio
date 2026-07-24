"""Cross-module ports. T07 provides the concrete render-ingestion implementation."""

from __future__ import annotations

from typing import Protocol
from uuid import UUID


class AssetIngestionPort(Protocol):
    """M2 hands completed Compute metadata to M3 without importing M3 repositories or S3 adapters."""

    async def create_from_completed_render(self, *, render_job_id: UUID) -> None: ...


class EntitlementReader(Protocol):
    """M5-owned authorization answer. M3 receives no order/payment data."""

    async def has_active_entitlement(self, *, user_id: UUID, asset_id: UUID) -> bool: ...


class DenyEntitlementReader:
    """T08 permits owner download before M5/T11 wires purchaser entitlement reads."""

    async def has_active_entitlement(self, *, user_id: UUID, asset_id: UUID) -> bool:
        return False
