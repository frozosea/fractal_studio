"""Durable render quota (T06) and ephemeral preview rate limit."""

from __future__ import annotations

from uuid import UUID

from app.infrastructure.redis.quota_store import RedisPreviewQuotaStore


class PreviewRateLimiter:
    """Small application port; preview path has no PostgreSQL/S3/outbox side effect."""

    def __init__(self, store: RedisPreviewQuotaStore | None = None) -> None:
        self._store = store or RedisPreviewQuotaStore()

    async def allow(self, user_id: UUID) -> bool:
        return await self._store.consume(user_id)
