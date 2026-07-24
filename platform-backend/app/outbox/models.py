"""Versioned, secret-free durable event DTOs and worker ports."""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime
from typing import Any, Awaitable, Callable, Protocol
from uuid import UUID


@dataclass(frozen=True, slots=True)
class NewOutboxEvent:
    event_type: str
    aggregate_type: str
    aggregate_id: UUID
    idempotency_key: str
    payload: dict[str, Any]
    schema_version: int = 1
    available_at: datetime | None = None
    causation_request_id: str | None = None


@dataclass(frozen=True, slots=True)
class OutboxEvent:
    id: UUID
    event_type: str
    aggregate_type: str
    aggregate_id: UUID
    idempotency_key: str
    payload: dict[str, Any]
    schema_version: int
    attempt_count: int
    retry_count: int
    available_at: datetime
    causation_request_id: str | None


class RetryableOutboxError(Exception):
    """Handler signals a bounded retry using a stable, non-secret error code."""

    def __init__(self, code: str) -> None:
        self.code = code
        super().__init__(code)


class RescheduleOutboxEvent(Exception):
    """Normal deferred work: reuse the same event row without consuming retry attempts."""

    def __init__(self, *, delay_seconds: int) -> None:
        self.delay_seconds = delay_seconds
        super().__init__("reschedule")


OutboxHandler = Callable[[OutboxEvent], Awaitable[None]]


class DueWorkReader(Protocol):
    """Domain-owned selector; worker calls it periodically but does not know domain predicates."""

    async def schedule_due_work(self, service: "OutboxService") -> int: ...


class OutboxService(Protocol):
    async def append(self, event: NewOutboxEvent) -> UUID: ...
