"""M3 safe read port for M4. Storage keys stay inside this package."""

from __future__ import annotations

import asyncio
from dataclasses import dataclass
from uuid import UUID

from app.assets import repository
from app.assets.ports import AssetPreview, AssetReader, OwnedAsset, PublishableAsset
from app.core.config import Settings, get_settings
from app.core.db import get_engine
from app.infrastructure.storage.object_storage import ObjectStorage


@dataclass(frozen=True, slots=True)
class _PreviewKeys:
    thumbnail: str | None
    watermarked_preview: str | None
    video_poster: str | None


class AssetReadService(AssetReader):
    """Repository-backed M3 adapter with short-lived browser derivative URLs."""

    def __init__(
        self, *, object_storage: ObjectStorage | None = None, settings: Settings | None = None
    ) -> None:
        self._storage = object_storage or ObjectStorage(settings)
        self._settings = settings or get_settings()

    async def find_owned_asset(self, *, asset_id: UUID, owner_id: UUID) -> OwnedAsset | None:
        async with get_engine().connect() as connection:
            record = await repository.find_owned_read_record(
                connection, asset_id=asset_id, owner_id=owner_id
            )
        return self._owned(record) if record is not None else None

    async def find_public_preview(self, *, asset_id: UUID) -> AssetPreview | None:
        async with get_engine().connect() as connection:
            record = await repository.find_ready_preview_record(connection, asset_id=asset_id)
        if record is None:
            return None
        return await self._preview(record)

    async def find_publishable_asset(
        self, *, asset_id: UUID, owner_id: UUID
    ) -> PublishableAsset | None:
        async with get_engine().connect() as connection:
            record = await repository.find_publishable_read_record(
                connection, asset_id=asset_id, owner_id=owner_id
            )
        if record is None:
            return None
        preview = await self._preview(record)
        if not self._has_required_preview(record.media_type, preview):
            return None
        return PublishableAsset(asset=self._owned(record), preview=preview)

    @staticmethod
    def _owned(record: repository.AssetReadRecord) -> OwnedAsset:
        return OwnedAsset(
            asset_id=record.id,
            owner_id=record.owner_id,
            media_type=record.media_type,
            status=record.status,
            visibility=record.visibility,
        )

    async def _preview(self, record: repository.AssetReadRecord) -> AssetPreview:
        keys = _PreviewKeys(
            thumbnail=record.derivative_keys.get("thumbnail"),
            watermarked_preview=record.derivative_keys.get("watermarked_preview"),
            video_poster=record.derivative_keys.get("video_poster"),
        )
        urls = await asyncio.gather(
            *(
                self._storage.create_signed_get_url(
                    object_key=key, expires_seconds=self._settings.public_preview_ttl_seconds
                )
                if key is not None
                else _none()
                for key in (keys.thumbnail, keys.watermarked_preview, keys.video_poster)
            )
        )
        return AssetPreview(
            asset_id=record.id,
            media_type=record.media_type,
            thumbnail_url=urls[0],
            watermarked_preview_url=urls[1],
            video_poster_url=urls[2],
        )

    @staticmethod
    def _has_required_preview(media_type: str, preview: AssetPreview) -> bool:
        if media_type in {"image", "mesh"}:
            return bool(preview.thumbnail_url and preview.watermarked_preview_url)
        if media_type == "video":
            return bool(preview.video_poster_url and preview.watermarked_preview_url)
        return False


async def _none() -> None:
    return None
