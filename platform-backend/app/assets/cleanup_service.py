"""M3 orphan-object cleanup. Worker owns retries; service owns storage rule."""

from __future__ import annotations

from uuid import UUID

from app.assets import repository
from app.core.db import get_engine
from app.infrastructure.storage.object_storage import ObjectStorage
from app.outbox.models import OutboxEvent, RetryableOutboxError


class AssetCleanupService:
    def __init__(self, *, storage: ObjectStorage | None = None) -> None:
        self._storage = storage or ObjectStorage()

    async def delete_orphan(self, event: OutboxEvent) -> None:
        try:
            task_id = UUID(str(event.payload["cleanupTaskId"]))
        except (KeyError, TypeError, ValueError) as error:
            raise RetryableOutboxError("invalid_cleanup_event") from error
        async with get_engine().begin() as connection:
            object_keys = await repository.lock_cleanup_task(connection, task_id=task_id)
        if object_keys is None:
            return
        try:
            for object_key in object_keys:
                await self._storage.delete(object_key=object_key)
        except Exception as error:
            raise RetryableOutboxError("object_cleanup_failed") from error
        async with get_engine().begin() as connection:
            await repository.mark_cleanup_done(connection, task_id=task_id)
