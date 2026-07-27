"""M3 orphan-object cleanup. Worker owns retries; service owns storage rule."""

from __future__ import annotations

from uuid import UUID

from app.assets import repository
from app.core import audit_writer
from app.core.config import Settings, get_settings
from app.core.db import get_engine
from app.infrastructure.storage.object_storage import ObjectStorage
from app.outbox.models import NewOutboxEvent, OutboxEvent, OutboxService, RetryableOutboxError
from app.outbox.service import TransactionalOutboxService


async def queue_object_cleanup(
    connection,
    *,
    object_keys: list[str],
    causation_request_id: str | None = None,
) -> UUID | None:
    """Durably queue M3 object cleanup in producer transaction; no S3 call here."""
    if not object_keys:
        return None
    task_id = await repository.create_orphan_cleanup_task(connection, object_keys=sorted(set(object_keys)))
    await TransactionalOutboxService(connection).append(
        NewOutboxEvent(
            event_type="cleanup.expired.v1",
            aggregate_type="storage_cleanup_task",
            aggregate_id=task_id,
            idempotency_key="initial",
            payload={"scope": "asset_storage", "cleanupTaskId": str(task_id)},
            causation_request_id=causation_request_id,
        )
    )
    return task_id


class AssetCleanupService:
    def __init__(
        self, *, storage: ObjectStorage | None = None, settings: Settings | None = None
    ) -> None:
        self._storage = storage or ObjectStorage()
        self._settings = settings or get_settings()

    async def cleanup_expired_objects(self, event: OutboxEvent) -> None:
        try:
            if event.payload.get("scope") != "asset_storage" and event.event_type != "asset.cleanup_orphan.v1":
                return
            task_id = UUID(str(event.payload["cleanupTaskId"]))
        except (KeyError, TypeError, ValueError) as error:
            raise RetryableOutboxError("invalid_cleanup_event") from error
        async with get_engine().begin() as connection:
            object_keys = await repository.lock_cleanup_task(connection, task_id=task_id)
        if object_keys is None:
            return
        try:
            async with get_engine().connect() as connection:
                referenced = await repository.referenced_object_keys(connection, object_keys=object_keys)
            for object_key in object_keys:
                if object_key in referenced:
                    continue
                await self._storage.delete(object_key=object_key)
        except Exception as error:
            raise RetryableOutboxError("object_cleanup_failed") from error
        async with get_engine().begin() as connection:
            await repository.mark_cleanup_done(connection, task_id=task_id)

    async def dead_letter(self, event: OutboxEvent, error_code: str) -> None:
        try:
            if event.payload.get("scope") != "asset_storage" and event.event_type != "asset.cleanup_orphan.v1":
                return
            task_id = UUID(str(event.payload["cleanupTaskId"]))
        except (KeyError, TypeError, ValueError):
            return
        async with get_engine().begin() as connection:
            await repository.mark_cleanup_retry(
                connection,
                task_id=task_id,
                error_code=error_code,
                delay_seconds=self._settings.asset_cleanup_retry_delay_seconds,
            )
            await audit_writer.record_system_action(
                connection,
                action="asset.cleanup_dead_lettered",
                subject_type="storage_cleanup_task",
                subject_id=task_id,
                request_id_value=event.causation_request_id or f"outbox:{event.id}",
                metadata={"errorCode": error_code},
            )


class AssetCleanupScheduler:
    """M3 predicate used by M7 periodic scheduler; worker owns no cleanup policy."""

    async def schedule_due_work(self, service: OutboxService) -> int:
        if not isinstance(service, TransactionalOutboxService):
            raise TypeError("asset cleanup scheduler requires a transaction-bound outbox service")
        tasks = await repository.claim_due_cleanup_tasks(service.connection)
        for task in tasks:
            await service.append(
                NewOutboxEvent(
                    event_type="cleanup.expired.v1",
                    aggregate_type="storage_cleanup_task",
                    aggregate_id=task.id,
                    idempotency_key=f"retry:{task.retry_generation}",
                    payload={"scope": "asset_storage", "cleanupTaskId": str(task.id)},
                )
            )
        return len(tasks)
