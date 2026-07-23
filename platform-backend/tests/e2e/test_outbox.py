"""Live PostgreSQL outbox verification.

Run with E2E_DATABASE_URL=postgresql+asyncpg://... pytest tests/e2e/test_outbox.py.
"""

from __future__ import annotations

import os
import uuid
from datetime import UTC, datetime, timedelta

import pytest
from sqlalchemy import text
from sqlalchemy.ext.asyncio import create_async_engine

from app.outbox import repository
from app.outbox.models import NewOutboxEvent
from app.outbox.service import TransactionalOutboxService


DATABASE_URL = os.getenv("E2E_DATABASE_URL")


@pytest.mark.asyncio
@pytest.mark.skipif(not DATABASE_URL, reason="set E2E_DATABASE_URL for live PostgreSQL outbox checks")
async def test_outbox_append_claim_retry_dead_letter_and_rollback() -> None:
    engine = create_async_engine(DATABASE_URL)
    aggregate_id = uuid.uuid4()
    event = NewOutboxEvent(
        event_type="test.outbox.v1",
        aggregate_type="test_aggregate",
        aggregate_id=aggregate_id,
        idempotency_key=f"e2e-{aggregate_id}",
        payload={"aggregateId": str(aggregate_id)},
        causation_request_id="e2e-outbox",
    )
    try:
        async with engine.begin() as connection:
            service = TransactionalOutboxService(connection)
            event_id = await service.append(event)
            assert event_id == await service.append(event)

        async with engine.begin() as connection:
            first_claim = await repository.claim_due_batch(
                connection, worker_id="worker-a", lease_seconds=1, batch_size=10
            )
        claimed = next(item for item in first_claim if item.id == event_id)
        assert claimed.attempt_count == 1

        async with engine.begin() as connection:
            assert await repository.reschedule_or_dead_letter(
                connection,
                event_id=event_id,
                worker_id="worker-a",
                error_code="test_transient",
                max_attempts=2,
                backoff_seconds=0,
            ) == "pending"

        async with engine.begin() as connection:
            second_claim = await repository.claim_due_batch(
                connection, worker_id="worker-b", lease_seconds=1, batch_size=10
            )
        assert next(item for item in second_claim if item.id == event_id).attempt_count == 2

        async with engine.begin() as connection:
            assert await repository.reschedule_or_dead_letter(
                connection,
                event_id=event_id,
                worker_id="worker-b",
                error_code="test_terminal",
                max_attempts=2,
                backoff_seconds=0,
            ) == "dead"

        async with engine.connect() as connection:
            state = await connection.execute(
                text("SELECT status::text, last_error_code, dead_at FROM outbox_events WHERE id = :id"),
                {"id": event_id},
            )
            row = state.one()
            assert row[0] == "dead"
            assert row[1] == "test_terminal"
            assert row[2] is not None

        rollback_event = NewOutboxEvent(
            event_type="test.rollback.v1",
            aggregate_type="test_aggregate",
            aggregate_id=uuid.uuid4(),
            idempotency_key=f"rollback-{uuid.uuid4()}",
            payload={"immutableId": "test"},
            available_at=datetime.now(UTC) + timedelta(days=1),
        )
        with pytest.raises(RuntimeError):
            async with engine.begin() as connection:
                await TransactionalOutboxService(connection).append(rollback_event)
                raise RuntimeError("force producer rollback")
        async with engine.connect() as connection:
            count = await connection.scalar(
                text("SELECT count(*) FROM outbox_events WHERE idempotency_key = :key"),
                {"key": rollback_event.idempotency_key},
            )
            assert count == 0

        expired_lease_event = NewOutboxEvent(
            event_type="test.expired_lease.v1",
            aggregate_type="test_aggregate",
            aggregate_id=uuid.uuid4(),
            idempotency_key=f"expired-{uuid.uuid4()}",
            payload={"immutableId": "lease-test"},
        )
        async with engine.begin() as connection:
            expired_lease_id = await TransactionalOutboxService(connection).append(expired_lease_event)
            claimed = await repository.claim_due_batch(
                connection, worker_id="worker-lease-a", lease_seconds=60, batch_size=10
            )
            assert any(item.id == expired_lease_id for item in claimed)
            await connection.execute(
                text("UPDATE outbox_events SET lease_until = now() - interval '1 second' WHERE id = :id"),
                {"id": expired_lease_id},
            )
        async with engine.begin() as connection:
            reclaimed = await repository.claim_due_batch(
                connection, worker_id="worker-lease-b", lease_seconds=60, batch_size=10
            )
            reclaimed_event = next(item for item in reclaimed if item.id == expired_lease_id)
            assert reclaimed_event.attempt_count == 2
            assert await repository.mark_done(
                connection, event_id=expired_lease_id, worker_id="worker-lease-b"
            )
    finally:
        await engine.dispose()
