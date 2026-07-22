from __future__ import annotations

import uuid
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from typing import Any

from sqlalchemy import or_, select
from sqlalchemy.ext.asyncio import AsyncSession

from .models import OutboxEvent


@dataclass(frozen=True, slots=True)
class ClaimedEvent:
    id: uuid.UUID
    event_type: str
    aggregate_id: uuid.UUID
    payload: dict[str, Any]
    attempt_count: int


class OutboxRepository:
    async def claim_due_batch(self, session: AsyncSession, *, limit: int, lease_seconds: int) -> list[ClaimedEvent]:
        now = datetime.now(timezone.utc)
        statement = (
            select(OutboxEvent)
            .where(
                OutboxEvent.status.in_(["pending", "leased"]),
                OutboxEvent.available_at <= now,
                or_(OutboxEvent.lease_until.is_(None), OutboxEvent.lease_until < now),
            )
            .order_by(OutboxEvent.available_at, OutboxEvent.created_at)
            .limit(limit)
            .with_for_update(skip_locked=True)
        )
        events = list((await session.scalars(statement)).all())
        lease_until = now + timedelta(seconds=lease_seconds)
        claimed = []
        for event in events:
            event.status = "leased"
            event.lease_until = lease_until
            event.attempt_count += 1
            claimed.append(ClaimedEvent(
                event.id, event.event_type, event.aggregate_id,
                dict(event.payload_json), event.attempt_count,
            ))
        return claimed

    async def mark_done(self, session: AsyncSession, event_id: uuid.UUID) -> None:
        event = await session.get(OutboxEvent, event_id, with_for_update=True)
        if event is None:
            return
        event.status = "completed"
        event.completed_at = datetime.now(timezone.utc)
        event.lease_until = None
        event.last_error = None

    async def reschedule(self, session: AsyncSession, event_id: uuid.UUID, error: str, attempt: int) -> None:
        event = await session.get(OutboxEvent, event_id, with_for_update=True)
        if event is None:
            return
        delay = min(300, 2 ** min(attempt, 8))
        event.status = "pending"
        event.available_at = datetime.now(timezone.utc) + timedelta(seconds=delay)
        event.lease_until = None
        event.last_error = error[:2000]

