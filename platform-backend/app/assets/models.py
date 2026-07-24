"""Safe M3 browser DTOs. Object keys and Compute identifiers never appear here."""

from __future__ import annotations

from datetime import datetime
from typing import Literal
from uuid import UUID

from pydantic import BaseModel, ConfigDict, Field


class AssetFileView(BaseModel):
    model_config = ConfigDict(populate_by_name=True)

    purpose: Literal["master", "thumbnail", "watermarked_preview", "video_poster"]
    media_type: str = Field(alias="mediaType")
    size_bytes: int = Field(alias="sizeBytes")


class AssetView(BaseModel):
    model_config = ConfigDict(populate_by_name=True)

    id: UUID
    recipe_id: UUID = Field(alias="recipeId")
    media_type: Literal["image", "video", "mesh"] = Field(alias="mediaType")
    status: Literal["processing", "ready", "failed", "deleted"]
    visibility: Literal["private", "hidden"]
    created_at: datetime = Field(alias="createdAt")
    files: list[AssetFileView]


class AssetVisibilityInput(BaseModel):
    visibility: Literal["private", "hidden"]


class DownloadUrlView(BaseModel):
    url: str
    expires_at: datetime = Field(alias="expiresAt")
