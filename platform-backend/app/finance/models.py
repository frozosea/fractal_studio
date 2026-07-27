"""M6 immutable journal values. These are internal, never browser DTOs."""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime
from decimal import Decimal
from uuid import UUID

from pydantic import BaseModel, ConfigDict, Field


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


@dataclass(frozen=True, slots=True)
class PayoutRequestRecord:
    id: UUID
    creator_id: UUID
    amount: Decimal
    currency: str
    qr_object_key: str
    status: str
    external_reference: str | None
    rejection_reason: str | None
    operator_user_id: UUID | None
    created_at: datetime
    paid_at: datetime | None
    rejected_at: datetime | None
    cancelled_at: datetime | None
    qr_deleted_at: datetime | None
    creator_email: str | None = None
    creator_handle: str | None = None
    operator_email: str | None = None


class PayoutRequestView(BaseModel):
    id: UUID
    amount: Decimal
    currency: str
    status: str
    created_at: datetime = Field(alias="createdAt")
    paid_at: datetime | None = Field(default=None, alias="paidAt")
    rejection_reason: str | None = Field(default=None, alias="rejectionReason")

    model_config = ConfigDict(populate_by_name=True)


class CreatorBalanceView(BaseModel):
    available_amount: Decimal = Field(alias="availableAmount")
    reserved_amount: Decimal = Field(alias="reservedAmount")
    currency: str

    model_config = ConfigDict(populate_by_name=True)


class InternalPayoutRequestView(PayoutRequestView):
    creator: dict[str, str | None]
    qr_url: str | None = Field(default=None, alias="qrUrl")
    qr_expires_at: datetime | None = Field(default=None, alias="qrExpiresAt")
    operator: dict[str, str] | None = None
    external_reference: str | None = Field(default=None, alias="externalReference")


class PayoutSettlementInput(BaseModel):
    external_reference: str = Field(min_length=1, max_length=160, alias="externalReference")

    model_config = ConfigDict(populate_by_name=True)

    def normalized_reference(self) -> str:
        value = self.external_reference.strip()
        if not value:
            raise ValueError("invalid_external_reference")
        return value


class PayoutRejectInput(BaseModel):
    reason: str = Field(min_length=1, max_length=1000)

    def normalized_reason(self) -> str:
        value = self.reason.strip()
        if not value:
            raise ValueError("invalid_rejection_reason")
        return value
