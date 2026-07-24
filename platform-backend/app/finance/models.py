"""M6 immutable journal values. These are internal, never browser DTOs."""

from __future__ import annotations

from dataclasses import dataclass
from decimal import Decimal
from uuid import UUID


@dataclass(frozen=True, slots=True)
class FrozenOrderItem:
    """M5-owned immutable values read from ``order_items`` under a row lock."""

    id: UUID
    creator_id: UUID
    price_amount: Decimal
    creator_amount: Decimal
    platform_fee_amount: Decimal
    currency: str


@dataclass(frozen=True, slots=True)
class CreatorBalance:
    creator_id: UUID
    available_amount: Decimal
    reserved_amount: Decimal
    currency: str


@dataclass(frozen=True, slots=True)
class LedgerEntry:
    id: UUID
    creator_id: UUID | None
    order_item_id: UUID | None
    payout_request_id: UUID | None
    account: str
    signed_amount: Decimal
    currency: str
    entry_type: str


class LedgerInvariantError(ValueError):
    """Frozen-order or CNY invariant failed before any journal write."""


class InsufficientCreatorBalanceError(ValueError):
    """A reversal cannot consume creator funds already reserved/paid out."""
