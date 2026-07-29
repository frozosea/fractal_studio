"""Durable render quota (T06) and ephemeral preview rate limit."""

from __future__ import annotations

from dataclasses import dataclass
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


@dataclass(frozen=True)
class ExportAllowance:
    """Free-export budget. `limit`/`remaining` are None for members: unmetered."""

    member: bool
    limit: int | None
    used: int
    remaining: int | None


class RenderQuotaService:
    """PostgreSQL is render-quota authority; Redis is never read on this path."""

    def __init__(self, settings: Settings | None = None) -> None:
        self._settings = settings or get_settings()

    async def allowance(self, connection: AsyncConnection, *, owner_id: UUID) -> ExportAllowance:
        """How much of the free export allowance is left. Members are unmetered."""
        if await render_job_repository.has_active_membership(connection, user_id=owner_id):
            return ExportAllowance(member=True, limit=None, used=0, remaining=None)
        used = await render_job_repository.count_lifetime_exports(connection, owner_id=owner_id)
        limit = self._settings.render_free_export_limit
        return ExportAllowance(member=False, limit=limit, used=used, remaining=max(0, limit - used))

    async def reserve(
        self, connection: AsyncConnection, *, owner_id: UUID, job_id: UUID, output_kind: str = "image"
    ) -> None:
        active_units = await render_job_repository.lock_user_reserved_units(connection, owner_id=owner_id)
        # The owner row is locked by the call above, so two exports racing each
        # other cannot both read the same count and slip past the last free
        # slot. The job being reserved is already inserted and therefore part of
        # `used`, hence `>` rather than `>=`.
        if output_kind == "image" and not await render_job_repository.has_active_membership(
            connection, user_id=owner_id
        ):
            used = await render_job_repository.count_lifetime_exports(connection, owner_id=owner_id)
            if used > self._settings.render_free_export_limit:
                raise HTTPException(status_code=status.HTTP_402_PAYMENT_REQUIRED, detail="export_quota_exhausted")
        if active_units >= self._settings.render_quota_max_active:
            raise HTTPException(status_code=status.HTTP_429_TOO_MANY_REQUESTS, detail="quota_exceeded")
        await render_job_repository.create_reservation(connection, owner_id=owner_id, job_id=job_id)

    async def release(self, connection: AsyncConnection, *, job_id: UUID) -> bool:
        return await render_job_repository.release_reservation(connection, job_id=job_id)
