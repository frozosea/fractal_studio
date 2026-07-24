"""M4 request/response DTOs. Money always travels as a decimal string."""

from __future__ import annotations

from datetime import datetime
from decimal import Decimal, InvalidOperation
from typing import Any, Literal
from uuid import UUID

from pydantic import BaseModel, ConfigDict, Field, field_validator, model_validator


def _money(value: Decimal | str) -> Decimal:
    try:
        amount = Decimal(value)
    except (InvalidOperation, ValueError) as error:
        raise ValueError("invalid CNY amount") from error
    if not amount.is_finite() or amount < Decimal("0.01") or amount.as_tuple().exponent < -2:
        raise ValueError("price must be a CNY amount with at most two decimal places")
    return amount.quantize(Decimal("0.01"))


def normalize_tags(tags: list[str]) -> list[str]:
    normalized = [tag.strip().lower() for tag in tags]
    if len(normalized) > 10 or any(not 1 <= len(tag) <= 32 for tag in normalized):
        raise ValueError("tags must contain at most 10 non-empty values of 1-32 characters")
    if len(set(normalized)) != len(normalized):
        raise ValueError("tags must be unique")
    return normalized


class LicenceOfferInput(BaseModel):
    code: str = Field(min_length=1, max_length=80)
    terms_version: str = Field(min_length=1, max_length=80, alias="termsVersion")

    model_config = ConfigDict(populate_by_name=True)

    @field_validator("code", "terms_version")
    @classmethod
    def normalize(cls, value: str) -> str:
        result = value.strip().lower()
        if not result:
            raise ValueError("licence value cannot be blank")
        return result


class ListingCreateInput(BaseModel):
    asset_id: UUID = Field(alias="assetId")
    title: str = Field(min_length=1, max_length=120)
    description: str = Field(default="", max_length=4000)
    tags: list[str] = Field(default_factory=list)
    price: Decimal
    licence_offer: LicenceOfferInput = Field(alias="licenceOffer")

    model_config = ConfigDict(populate_by_name=True)

    _valid_price = field_validator("price")(_money)
    _valid_tags = field_validator("tags")(normalize_tags)

    @field_validator("title")
    @classmethod
    def title_not_blank(cls, value: str) -> str:
        result = value.strip()
        if not result:
            raise ValueError("title cannot be blank")
        return result


class ListingUpdateInput(BaseModel):
    title: str | None = Field(default=None, min_length=1, max_length=120)
    description: str | None = Field(default=None, max_length=4000)
    tags: list[str] | None = None
    price: Decimal | None = None
    licence_offer: LicenceOfferInput | None = Field(default=None, alias="licenceOffer")

    model_config = ConfigDict(populate_by_name=True)

    _valid_price = field_validator("price")(_money)
    _valid_tags = field_validator("tags")(lambda value: normalize_tags(value) if value is not None else value)

    @field_validator("title")
    @classmethod
    def update_title_not_blank(cls, value: str | None) -> str | None:
        if value is None:
            return None
        result = value.strip()
        if not result:
            raise ValueError("title cannot be blank")
        return result

    @model_validator(mode="after")
    def has_update(self) -> "ListingUpdateInput":
        if not self.model_fields_set:
            raise ValueError("at least one field is required")
        return self


class CreatorView(BaseModel):
    id: UUID
    handle: str
    display_name: str = Field(alias="displayName")

    model_config = ConfigDict(populate_by_name=True)


class PreviewView(BaseModel):
    media_type: Literal["image", "video", "mesh"] = Field(alias="mediaType")
    thumbnail_url: str | None = Field(default=None, alias="thumbnailUrl")
    watermarked_preview_url: str | None = Field(default=None, alias="watermarkedPreviewUrl")
    video_poster_url: str | None = Field(default=None, alias="videoPosterUrl")

    model_config = ConfigDict(populate_by_name=True)


class LicenceOfferView(BaseModel):
    id: UUID
    code: str
    terms_version: str = Field(alias="termsVersion")
    terms: dict[str, Any]

    model_config = ConfigDict(populate_by_name=True)


class ListingView(BaseModel):
    id: UUID
    asset_id: UUID = Field(alias="assetId")
    creator: CreatorView
    status: Literal["draft", "published", "unpublished"]
    title: str
    description: str
    tags: list[str]
    price: Decimal
    currency: Literal["CNY"]
    published_at: datetime | None = Field(default=None, alias="publishedAt")
    preview: PreviewView | None = None
    licence_offer: LicenceOfferView = Field(alias="licenceOffer")

    model_config = ConfigDict(populate_by_name=True)


class FavoriteView(BaseModel):
    asset_id: UUID = Field(alias="assetId")
    created_at: datetime = Field(alias="createdAt")
    preview: PreviewView | None = None
    listing: ListingView | None = None

    model_config = ConfigDict(populate_by_name=True)


class SortQuery(BaseModel):
    value: Literal["newest", "price_asc", "price_desc", "relevance"] = "newest"
