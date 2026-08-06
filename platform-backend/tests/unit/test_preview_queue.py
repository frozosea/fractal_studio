from __future__ import annotations

from collections import defaultdict
from uuid import uuid4

import pytest

from app.core.config import Settings
from app.studio.preview_queue import RedisPreviewQueue


class MemoryRedis:
    """Small Redis double covering queue's latest-wins state transitions."""

    def __init__(self) -> None:
        self.values: dict[str, str] = {}
        self.hashes: dict[str, dict[str, str]] = defaultdict(dict)
        self.lists: dict[str, list[str]] = defaultdict(list)

    async def get(self, key: str) -> str | None:
        return self.values.get(key)

    async def set(self, key: str, value: str, **_kwargs: object) -> bool:
        self.values[key] = value
        return True

    async def setex(self, key: str, _seconds: int, value: str) -> bool:
        self.values[key] = value
        return True

    async def hset(self, key: str, field: str | None = None, value: str | None = None, *, mapping: dict[str, str] | None = None) -> int:
        if mapping is not None:
            self.hashes[key].update(mapping)
        elif field is not None and value is not None:
            self.hashes[key][field] = value
        return 1

    async def hgetall(self, key: str) -> dict[str, str]:
        return dict(self.hashes.get(key, {}))

    async def expire(self, *_args: object) -> bool:
        return True

    async def exists(self, key: str) -> int:
        return int(key in self.values)

    async def llen(self, key: str) -> int:
        return len(self.lists[key])

    async def lpush(self, key: str, value: str) -> int:
        self.lists[key].insert(0, value)
        return len(self.lists[key])

    async def brpop(self, key: str, **_kwargs: object) -> tuple[str, str] | None:
        if not self.lists[key]:
            return None
        return key, self.lists[key].pop()

    async def delete(self, key: str) -> int:
        return int(self.values.pop(key, None) is not None)

    async def zpopmin(self, *_args: object, **_kwargs: object) -> list[tuple[str, float]]:
        return []


@pytest.mark.asyncio
async def test_preview_queue_keeps_only_latest_pending_job_per_channel() -> None:
    queue = RedisPreviewQueue(Settings(database_url="postgresql+asyncpg://unused", session_secret="x" * 32))
    queue._redis = MemoryRedis()  # type: ignore[assignment]
    owner_id, session_id = uuid4(), uuid4()

    first_id, first_status = await queue.submit(
        owner_id=owner_id, session_id=session_id, channel="main", request={"scale": 3},
        spec_hash="first", width=320, height=320,
    )
    second_id, second_status = await queue.submit(
        owner_id=owner_id, session_id=session_id, channel="main", request={"scale": 1.5},
        spec_hash="second", width=320, height=320,
    )

    assert first_status == second_status == "queued"
    assert await queue.claim() is None  # Pops stale first job and promotes latest.
    claimed = await queue.claim()

    assert claimed is not None
    assert claimed.id == second_id
    assert claimed.request == {"scale": 1.5}
    stale = await queue.get(job_id=first_id, owner_id=owner_id, session_id=session_id)
    assert stale is not None
    assert stale.status == "stale"

    await queue.complete(claimed, b"png")
    cached_id, cached_status = await queue.submit(
        owner_id=owner_id, session_id=session_id, channel="main", request={"scale": 1.5},
        spec_hash="second", width=320, height=320,
    )
    cached = await queue.get(job_id=cached_id, owner_id=owner_id, session_id=session_id)
    assert cached_status == "completed"
    assert cached is not None
    assert await queue.image(cached) == b"png"
