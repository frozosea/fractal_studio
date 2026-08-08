"""Administrator request and response models."""

from __future__ import annotations

from datetime import datetime
from typing import Literal
from uuid import UUID

from pydantic import BaseModel, ConfigDict, Field, field_validator, model_validator

from app.marketplace.models import PreviewView


AdminRole = Literal["admin", "finance_operator"]
UserRole = Literal["admin", "creator", "finance_operator"]


class AdminUserUpdateInput(BaseModel):
    status: Literal["active", "disabled"] | None = None
    member: bool | None = None
    privileged_roles: list[AdminRole] | None = Field(default=None, alias="privilegedRoles")

    model_config = ConfigDict(populate_by_name=True)

    @field_validator("privileged_roles")
    @classmethod
    def unique_roles(cls, roles: list[AdminRole] | None) -> list[AdminRole] | None:
        if roles is not None and len(roles) != len(set(roles)):
            raise ValueError("privileged roles must be unique")
        return roles

    @model_validator(mode="after")
    def has_update(self) -> "AdminUserUpdateInput":
        if not self.model_fields_set:
            raise ValueError("at least one field is required")
        return self


class AdminUserView(BaseModel):
    id: UUID
    email: str
    status: Literal["active", "disabled"]
    roles: list[UserRole]
    member: bool
    creator_profile: dict[str, str] | None = Field(default=None, alias="creatorProfile")
    asset_count: int = Field(alias="assetCount")
    listing_count: int = Field(alias="listingCount")
    fulfilled_order_count: int = Field(alias="fulfilledOrderCount")
    created_at: datetime = Field(alias="createdAt")

    model_config = ConfigDict(populate_by_name=True)


class AdminListingModerationInput(BaseModel):
    action: Literal["unpublish", "archive"]
    reason: str = Field(min_length=1, max_length=500)

    @field_validator("reason")
    @classmethod
    def normalize_reason(cls, reason: str) -> str:
        value = reason.strip()
        if not value:
            raise ValueError("moderation reason cannot be blank")
        return value


class AdminListingView(BaseModel):
    id: UUID
    asset_id: UUID = Field(alias="assetId")
    creator_id: UUID = Field(alias="creatorId")
    creator_email: str = Field(alias="creatorEmail")
    creator_handle: str | None = Field(default=None, alias="creatorHandle")
    creator_display_name: str | None = Field(default=None, alias="creatorDisplayName")
    status: Literal["draft", "published", "unpublished", "archived"]
    title: str
    description: str
    tags: list[str]
    price: str
    currency: Literal["CNY"]
    media_type: Literal["image", "video", "mesh"] = Field(alias="mediaType")
    favorite_count: int = Field(alias="favoriteCount")
    sale_count: int = Field(alias="saleCount")
    created_at: datetime = Field(alias="createdAt")
    published_at: datetime | None = Field(default=None, alias="publishedAt")
    preview: PreviewView | None = None

    model_config = ConfigDict(populate_by_name=True)


class UserStatistics(BaseModel):
    total: int
    active: int
    disabled: int
    creators: int
    members: int
    admins: int
    new_last_30_days: int = Field(alias="newLast30Days")

    model_config = ConfigDict(populate_by_name=True)


class MarketStatistics(BaseModel):
    listings: int
    published: int
    draft: int
    unpublished: int
    archived: int
    ready_assets: int = Field(alias="readyAssets")
    favorites: int

    model_config = ConfigDict(populate_by_name=True)


class CommerceStatistics(BaseModel):
    orders: int
    fulfilled: int
    pending_payment: int = Field(alias="pendingPayment")
    payment_exceptions: int = Field(alias="paymentExceptions")
    marketplace_gross_cny: str = Field(alias="marketplaceGrossCny")
    membership_revenue_cny: str = Field(alias="membershipRevenueCny")
    creator_revenue_cny: str = Field(alias="creatorRevenueCny")
    platform_revenue_cny: str = Field(alias="platformRevenueCny")

    model_config = ConfigDict(populate_by_name=True)


class ComputeStatistics(BaseModel):
    render_jobs: int = Field(alias="renderJobs")
    active: int
    completed: int
    failed: int

    model_config = ConfigDict(populate_by_name=True)


class AdminComputeGpuView(BaseModel):
    name: str | None = None
    runtime: bool
    compute_capability: dict[str, int] | None = Field(default=None, alias="computeCapability")
    total_vram_bytes: int | None = Field(default=None, alias="totalVramBytes")
    free_vram_bytes: int | None = Field(default=None, alias="freeVramBytes")

    model_config = ConfigDict(populate_by_name=True)


class AdminComputeNodeView(BaseModel):
    node_key: str = Field(alias="nodeKey")
    state: Literal["active", "draining", "offline", "disabled"]
    healthy: bool
    max_durable_slots: int = Field(alias="maxDurableSlots")
    used_durable_slots: int = Field(alias="usedDurableSlots")
    max_preview_slots: int = Field(alias="maxPreviewSlots")
    used_preview_slots: int = Field(alias="usedPreviewSlots")
    renderer_version: str | None = Field(default=None, alias="rendererVersion")
    gpu: AdminComputeGpuView | None = None
    last_healthy_at: datetime | None = Field(default=None, alias="lastHealthyAt")
    last_assigned_at: datetime | None = Field(default=None, alias="lastAssignedAt")
    capabilities_at: datetime | None = Field(default=None, alias="capabilitiesAt")

    model_config = ConfigDict(populate_by_name=True)


class AdminStatisticsView(BaseModel):
    generated_at: datetime = Field(alias="generatedAt")
    users: UserStatistics
    market: MarketStatistics
    commerce: CommerceStatistics
    compute: ComputeStatistics

    model_config = ConfigDict(populate_by_name=True)
