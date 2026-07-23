"""Generic at-least-once dispatcher; event business rules stay in owning modules."""

from __future__ import annotations

import asyncio
import logging
import re
import signal
import uuid
from collections.abc import Iterable

from app.core.config import Settings, get_settings
from app.core.db import get_engine
from app.core.logging import worker_log
from app.core.request_context import request_id_var
from app.outbox import repository
from app.outbox.models import (
    DueWorkReader,
    OutboxEvent,
    OutboxHandler,
    RescheduleOutboxEvent,
    RetryableOutboxError,
)
from app.outbox.service import TransactionalOutboxService


_ERROR_CODE = re.compile(r"^[a-z][a-z0-9_]{0,119}$")


class HandlerRegistry:
    """Explicit registry prevents workers from importing or owning domain transitions."""

    def __init__(self) -> None:
        self._handlers: dict[str, OutboxHandler] = {}

    def register(self, event_type: str, handler: OutboxHandler) -> None:
        if event_type in self._handlers:
            raise ValueError(f"handler already registered for {event_type}")
        self._handlers[event_type] = handler

    async def dispatch(self, event: OutboxEvent) -> None:
        handler = self._handlers.get(event.event_type)
        if handler is None:
            raise RetryableOutboxError("handler_not_registered")
        await handler(event)


class OutboxWorker:
    def __init__(
        self,
        *,
        worker_id: str | None = None,
        handlers: HandlerRegistry | None = None,
        due_work_readers: Iterable[DueWorkReader] = (),
        settings: Settings | None = None,
    ) -> None:
        self._settings = settings or get_settings()
        self._worker_id = worker_id or f"outbox-{uuid.uuid4()}"
        self._handlers = handlers or HandlerRegistry()
        self._due_work_readers = tuple(due_work_readers)
        self._last_schedule_at = 0.0

    def _backoff_seconds(self, attempt_count: int) -> int:
        delay = self._settings.outbox_backoff_base_seconds * (2 ** max(0, attempt_count - 1))
        return min(delay, self._settings.outbox_backoff_max_seconds)

    async def run_scheduler_once(self) -> int:
        """Run domain readers in short transactions; readers own all due-work predicates."""
        scheduled = 0
        for reader in self._due_work_readers:
            async with get_engine().begin() as connection:
                scheduled += await reader.schedule_due_work(TransactionalOutboxService(connection))
        return scheduled

    async def poll_once(self) -> int:
        loop = asyncio.get_running_loop()
        if loop.time() - self._last_schedule_at >= self._settings.outbox_schedule_interval_seconds:
            scheduled = await self.run_scheduler_once()
            self._last_schedule_at = loop.time()
            if scheduled:
                worker_log(logging.INFO, "outbox scheduler appended events", count=scheduled)
        async with get_engine().begin() as connection:
            events = await repository.claim_due_batch(
                connection,
                worker_id=self._worker_id,
                lease_seconds=self._settings.outbox_lease_seconds,
                batch_size=self._settings.outbox_claim_batch_size,
            )
        for event in events:
            await self._dispatch_claimed(event)
        return len(events)

    async def _dispatch_claimed(self, event: OutboxEvent) -> None:
        token = request_id_var.set(event.causation_request_id or f"outbox:{event.id}")
        try:
            await self._handlers.dispatch(event)
        except RescheduleOutboxEvent as deferred:
            async with get_engine().begin() as connection:
                rescheduled = await repository.defer_same_event(
                    connection,
                    event_id=event.id,
                    worker_id=self._worker_id,
                    delay_seconds=deferred.delay_seconds,
                )
            worker_log(
                logging.INFO if rescheduled else logging.WARNING,
                "outbox event deferred" if rescheduled else "outbox lease lost before deferral",
                event_id=event.id,
                event_type=event.event_type,
            )
        except RetryableOutboxError as error:
            await self._fail(event, error.code)
        except Exception:
            await self._fail(event, "handler_unexpected_error")
        else:
            async with get_engine().begin() as connection:
                marked = await repository.mark_done(
                    connection, event_id=event.id, worker_id=self._worker_id
                )
            worker_log(
                logging.INFO if marked else logging.WARNING,
                "outbox event completed" if marked else "outbox lease lost before completion",
                event_id=event.id,
                event_type=event.event_type,
            )
        finally:
            request_id_var.reset(token)

    async def _fail(self, event: OutboxEvent, error_code: str) -> None:
        if not _ERROR_CODE.fullmatch(error_code):
            error_code = "handler_retry_failed"
        async with get_engine().begin() as connection:
            next_status = await repository.reschedule_or_dead_letter(
                connection,
                event_id=event.id,
                worker_id=self._worker_id,
                error_code=error_code,
                max_attempts=self._settings.outbox_max_attempts,
                backoff_seconds=self._backoff_seconds(event.attempt_count),
            )
        worker_log(
            logging.ERROR if next_status == "dead" else logging.WARNING,
            "outbox event dead-lettered" if next_status == "dead" else "outbox event rescheduled",
            event_id=event.id,
            event_type=event.event_type,
            error_code=error_code,
            next_status=next_status or "lease_lost",
        )

    async def run(self, stop_event: asyncio.Event) -> None:
        while not stop_event.is_set():
            claimed = await self.poll_once()
            if claimed == 0:
                try:
                    await asyncio.wait_for(stop_event.wait(), timeout=self._settings.outbox_poll_interval_seconds)
                except TimeoutError:
                    pass


async def run() -> None:
    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()
    for signal_name in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(signal_name, stop_event.set)
        except NotImplementedError:
            pass
    from app.studio.render_worker import build_render_handler_registry

    worker = OutboxWorker(handlers=build_render_handler_registry())
    worker_log(logging.INFO, "outbox worker started", worker_id=worker._worker_id)
    await worker.run(stop_event)
    worker_log(logging.INFO, "outbox worker stopped", worker_id=worker._worker_id)


if __name__ == "__main__":
    asyncio.run(run())
