"""M5 checkout input and secret-free buyer-facing views."""

from __future__ import annotations

from datetime import datetime
from decimal import Decimal
from typing import Literal
from uuid import UUID

from pydantic import BaseModel, ConfigDict, Field


class CheckoutInput(BaseModel):
    listing_id: UUID = Field(alias="listingId")
    licence_offer_id: UUID = Field(alias="licenceOfferId")
    channel: Literal["desktop_web", "mobile_web"]

    model_config = ConfigDict(populate_by_name=True, extra="forbid")


class OrderItemView(BaseModel):
    id: UUID
    listing_id: UUID = Field(alias="listingId")
    listing_version_id: UUID = Field(alias="listingVersionId")
    licence_offer_id: UUID = Field(alias="licenceOfferId")
    asset_id: UUID = Field(alias="assetId")
    creator_id: UUID = Field(alias="creatorId")
    price: Decimal
    currency: Literal["CNY"]
    commission_policy_version: str = Field(alias="commissionPolicyVersion")
    creator_amount: Decimal = Field(alias="creatorAmount")
    platform_fee_amount: Decimal = Field(alias="platformFeeAmount")

    model_config = ConfigDict(populate_by_name=True)


class OrderView(BaseModel):
    id: UUID
    status: Literal["pending_payment", "fulfilled", "closed", "payment_exception"]
    amount: Decimal
    currency: Literal["CNY"]
    paid_at: datetime | None = Field(default=None, alias="paidAt")
    created_at: datetime = Field(alias="createdAt")
    items: list[OrderItemView]

    model_config = ConfigDict(populate_by_name=True)


class PaymentAttemptView(BaseModel):
    id: UUID
    out_trade_no: str = Field(alias="outTradeNo")
    status: Literal["created", "pending", "succeeded", "closed", "failed"]
    expires_at: datetime = Field(alias="expiresAt")

    model_config = ConfigDict(populate_by_name=True)


class AlipayFormView(BaseModel):
    action: str
    method: Literal["POST"]
    fields: dict[str, str]


class PaymentStartView(BaseModel):
    order: OrderView
    payment_attempt: PaymentAttemptView = Field(alias="paymentAttempt")
    alipay_form: AlipayFormView = Field(alias="alipayForm")

    model_config = ConfigDict(populate_by_name=True)
