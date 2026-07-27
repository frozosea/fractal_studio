"""Atomic Redis-only preview rate-limit counter."""

from __future__ import annotations

from uuid import UUID

from redis import RedisError
from redis.asyncio import Redis

from app.core.config import Settings, get_settings


class PreviewQuotaUnavailable(RuntimeError):
    """Fail closed when the only allowed preview write cannot be made."""


class RedisPreviewQuotaStore:
    _INCREMENT_WITH_TTL = """
    local count = redis.call('INCR', KEYS[1])
    if count == 1 then redis.call('EXPIRE', KEYS[1], ARGV[1]) end
    return count
    """

    def __init__(self, settings: Settings | None = None) -> None:
        self._settings = settings or get_settings()

    async def consume(self, user_id: UUID) -> bool:
        key = f"preview-rate:v1:{user_id}"
        client: Redis = Redis.from_url(self._settings.redis_url, decode_responses=True)
        try:
            count = await client.eval(self._INCREMENT_WITH_TTL, 1, key, 60)
            return int(count) <= self._settings.preview_rate_limit_per_minute
        except (RedisError, OSError, ValueError) as error:
            raise PreviewQuotaUnavailable("preview_quota_unavailable") from error
        finally:
            await client.aclose()
