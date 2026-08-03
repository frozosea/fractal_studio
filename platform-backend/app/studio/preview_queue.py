"""Redis-backed latest-wins preview queue and short-lived PNG cache."""

from __future__ import annotations

import base64
import json
from dataclasses import dataclass
from uuid import UUID, uuid4

from redis import RedisError
from redis.asyncio import Redis

from app.core.config import Settings, get_settings


class PreviewQueueUnavailable(RuntimeError):
    pass


@dataclass(frozen=True, slots=True)
class PreviewJob:
    id: str
    owner_id: UUID
    session_id: UUID
    channel: str
    status: str
    request: dict[str, object]
    cache_key: str


class RedisPreviewQueue:
    _QUEUE = "studio-preview:v1:queue"
    _DEFERRED = "studio-preview:v1:deferred"

    def __init__(self, settings: Settings | None = None) -> None:
        self._settings = settings or get_settings()
        self._redis: Redis = Redis.from_url(self._settings.redis_url, decode_responses=True)

    @staticmethod
    def _job_key(job_id: str) -> str:
        return f"studio-preview:v1:job:{job_id}"

    @staticmethod
    def _latest_key(owner_id: UUID, session_id: UUID, channel: str) -> str:
        return f"studio-preview:v1:latest:{owner_id}:{session_id}:{channel}"

    @staticmethod
    def _queued_key(owner_id: UUID, session_id: UUID, channel: str) -> str:
        return f"studio-preview:v1:queued:{owner_id}:{session_id}:{channel}"

    @staticmethod
    def _cache_key(spec_hash: str, width: int, height: int) -> str:
        return f"studio-preview:v1:cache:{spec_hash}:{width}x{height}"

    async def submit(
        self,
        *,
        owner_id: UUID,
        session_id: UUID,
        channel: str,
        request: dict[str, object],
        spec_hash: str,
        width: int,
        height: int,
    ) -> tuple[str, str]:
        job_id = str(uuid4())
        cache_key = self._cache_key(spec_hash, width, height)
        latest_key = self._latest_key(owner_id, session_id, channel)
        queued_key = self._queued_key(owner_id, session_id, channel)
        try:
            cached = await self._redis.get(cache_key)
            status = "completed" if cached is not None else "queued"
            await self._redis.hset(
                self._job_key(job_id),
                mapping={
                    "ownerId": str(owner_id), "sessionId": str(session_id), "channel": channel,
                    "status": status, "request": json.dumps(request, separators=(",", ":")), "cacheKey": cache_key,
                },
            )
            await self._redis.expire(self._job_key(job_id), self._settings.preview_request_ttl_seconds)
            await self._redis.set(latest_key, job_id, ex=self._settings.preview_request_ttl_seconds)
            if cached is not None:
                return job_id, status
            if not await self._redis.exists(queued_key):
                if await self._redis.llen(self._QUEUE) < self._settings.preview_queue_max_pending:
                    await self._redis.set(queued_key, "1", ex=self._settings.preview_request_ttl_seconds)
                    await self._redis.lpush(self._QUEUE, job_id)
                else:
                    await self._redis.hset(self._job_key(job_id), "status", "deferred")
                    await self._redis.zadd(self._DEFERRED, {latest_key: 0})
                    status = "deferred"
            return job_id, status
        except RedisError as error:
            raise PreviewQueueUnavailable("preview_queue_unavailable") from error

    async def get(self, *, job_id: str, owner_id: UUID, session_id: UUID) -> PreviewJob | None:
        try:
            values = await self._redis.hgetall(self._job_key(job_id))
        except RedisError as error:
            raise PreviewQueueUnavailable("preview_queue_unavailable") from error
        if not values or values.get("ownerId") != str(owner_id) or values.get("sessionId") != str(session_id):
            return None
        try:
            return PreviewJob(
                id=job_id, owner_id=owner_id, session_id=session_id, channel=values["channel"],
                status=values["status"], request=json.loads(values["request"]), cache_key=values["cacheKey"],
            )
        except (KeyError, ValueError, TypeError):
            return None

    async def image(self, job: PreviewJob) -> bytes | None:
        if job.status != "completed":
            return None
        try:
            value = await self._redis.get(job.cache_key)
            return base64.b64decode(value) if value else None
        except (RedisError, ValueError) as error:
            raise PreviewQueueUnavailable("preview_queue_unavailable") from error

    async def claim(self, *, timeout_seconds: int = 1) -> PreviewJob | None:
        try:
            item = await self._redis.brpop(self._QUEUE, timeout=timeout_seconds)
            if item is None:
                await self.promote_deferred()
                return None
            _, job_id = item
            values = await self._redis.hgetall(self._job_key(job_id))
            if not values:
                return None
            owner_id, session_id, channel = UUID(values["ownerId"]), UUID(values["sessionId"]), values["channel"]
            latest_key = self._latest_key(owner_id, session_id, channel)
            queued_key = self._queued_key(owner_id, session_id, channel)
            latest = await self._redis.get(latest_key)
            if latest != job_id:
                await self._redis.hset(self._job_key(job_id), "status", "stale")
                if latest:
                    await self._redis.lpush(self._QUEUE, latest)
                return None
            await self._redis.delete(queued_key)
            await self._redis.hset(self._job_key(job_id), "status", "rendering")
            return await self.get(job_id=job_id, owner_id=owner_id, session_id=session_id)
        except (RedisError, KeyError, ValueError) as error:
            raise PreviewQueueUnavailable("preview_queue_unavailable") from error

    async def complete(self, job: PreviewJob, png: bytes) -> None:
        latest_key = self._latest_key(job.owner_id, job.session_id, job.channel)
        try:
            if await self._redis.get(latest_key) == job.id:
                await self._redis.setex(job.cache_key, self._settings.preview_cache_ttl_seconds, base64.b64encode(png).decode())
                await self._redis.hset(self._job_key(job.id), "status", "completed")
            else:
                await self._redis.hset(self._job_key(job.id), "status", "stale")
            await self.promote_deferred()
        except RedisError as error:
            raise PreviewQueueUnavailable("preview_queue_unavailable") from error

    async def retry(self, job: PreviewJob) -> None:
        try:
            latest_key = self._latest_key(job.owner_id, job.session_id, job.channel)
            if await self._redis.get(latest_key) == job.id:
                await self._redis.hset(self._job_key(job.id), "status", "queued")
                await self._redis.set(self._queued_key(job.owner_id, job.session_id, job.channel), "1", ex=self._settings.preview_request_ttl_seconds)
                await self._redis.lpush(self._QUEUE, job.id)
        except RedisError as error:
            raise PreviewQueueUnavailable("preview_queue_unavailable") from error

    async def promote_deferred(self) -> None:
        while await self._redis.llen(self._QUEUE) < self._settings.preview_queue_max_pending:
            item = await self._redis.zpopmin(self._DEFERRED, count=1)
            if not item:
                return
            latest_key, _ = item[0]
            latest = await self._redis.get(latest_key)
            if latest:
                await self._redis.lpush(self._QUEUE, latest)

    async def close(self) -> None:
        await self._redis.aclose()
