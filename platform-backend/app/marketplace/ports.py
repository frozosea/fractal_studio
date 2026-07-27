"""M5-facing immutable listing snapshot port."""

from __future__ import annotations

from dataclasses import dataclass
from decimal import Decimal
from typing import Protocol
from uuid import UUID


@dataclass(frozen=True, slots=True)
class PublishedOfferSnapshot:
    listing_id: UUID
    listing_version_id: UUID
    licence_offer_id: UUID
    asset_id: UUID
    creator_id: UUID
    price: Decimal
    currency: str
    listing_snapshot: dict[str, object]
    licence_snapshot: dict[str, object]


class ListingSnapshotReader(Protocol):
    async def find_published_offer(
        self, *, listing_id: UUID, licence_offer_id: UUID
    ) -> PublishedOfferSnapshot | None: ...
