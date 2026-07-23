"""Durable render quota (T06) and ephemeral preview rate limit."""

from __future__ import annotations

from uuid import UUID

from fastapi import HTTPException, status
from sqlalchemy.ext.asyncio import AsyncConnection

from app.core.config import Settings, get_settings
from app.infrastructure.redis.quota_store import RedisPreviewQuotaStore
from app.studio import render_job_repository


class PreviewRateLimiter:
    """Small application port; preview path has no PostgreSQL/S3/outbox side effect."""

    def __init__(self, store: RedisPreviewQuotaStore | None = None) -> None:
        self._store = store or RedisPreviewQuotaStore()

    async def allow(self, user_id: UUID) -> bool:
        return await self._store.consume(user_id)


class RenderQuotaService:
    """PostgreSQL is render-quota authority; Redis is never read on this path."""

    def __init__(self, settings: Settings | None = None) -> None:
        self._settings = settings or get_settings()

    async def reserve(self, connection: AsyncConnection, *, owner_id: UUID, job_id: UUID) -> None:
        active_units = await render_job_repository.lock_user_reserved_units(connection, owner_id=owner_id)
        if active_units >= self._settings.render_quota_max_active:
            raise HTTPException(status_code=status.HTTP_429_TOO_MANY_REQUESTS, detail="quota_exceeded")
        await render_job_repository.create_reservation(connection, owner_id=owner_id, job_id=job_id)

    async def release(self, connection: AsyncConnection, *, job_id: UUID) -> bool:
        return await render_job_repository.release_reservation(connection, job_id=job_id)
