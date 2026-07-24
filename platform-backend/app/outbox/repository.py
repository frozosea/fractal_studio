"""PostgreSQL outbox persistence: unique append, SKIP LOCKED claim and terminal transitions."""

from __future__ import annotations

import json
from uuid import UUID, uuid4

from sqlalchemy import text
from sqlalchemy.ext.asyncio import AsyncConnection

from app.outbox.models import NewOutboxEvent, OutboxEvent


def _event_from_row(row: dict[str, object]) -> OutboxEvent:
    return OutboxEvent(
        id=row["id"],
        event_type=str(row["event_type"]),
        aggregate_type=str(row["aggregate_type"]),
        aggregate_id=row["aggregate_id"],
        idempotency_key=str(row["idempotency_key"]),
        payload=row["payload_json"],
        schema_version=int(row["schema_version"]),
        attempt_count=int(row["attempt_count"]),
        retry_count=int(row["retry_count"]),
        available_at=row["available_at"],
        causation_request_id=row["causation_request_id"],
    )


async def append(connection: AsyncConnection, event: NewOutboxEvent) -> UUID:
    """Append in producer transaction; a duplicate returns its original durable ID."""
    event_id = uuid4()
    result = await connection.execute(
        text(
            """
            INSERT INTO outbox_events (
              id, event_type, schema_version, aggregate_type, aggregate_id, payload_json,
              idempotency_key, status, available_at, attempt_count, retry_count, causation_request_id
            ) VALUES (
              :id, :event_type, :schema_version, :aggregate_type, :aggregate_id,
              CAST(:payload_json AS jsonb), :idempotency_key, 'pending',
              COALESCE(:available_at, now()), 0, 0, :causation_request_id
            )
            ON CONFLICT (event_type, aggregate_id, idempotency_key) DO NOTHING
            RETURNING id
            """
        ),
        {
            "id": event_id,
            "event_type": event.event_type,
            "schema_version": event.schema_version,
            "aggregate_type": event.aggregate_type,
            "aggregate_id": event.aggregate_id,
            "payload_json": json.dumps(event.payload, sort_keys=True, separators=(",", ":")),
            "idempotency_key": event.idempotency_key,
            "available_at": event.available_at,
            "causation_request_id": event.causation_request_id,
        },
    )
    inserted_id = result.scalar_one_or_none()
    if inserted_id is not None:
        return inserted_id
    existing = await connection.execute(
        text(
            """
            SELECT id FROM outbox_events
            WHERE event_type = :event_type AND aggregate_id = :aggregate_id
              AND idempotency_key = :idempotency_key
            """
        ),
        {
            "event_type": event.event_type,
            "aggregate_id": event.aggregate_id,
            "idempotency_key": event.idempotency_key,
        },
    )
    return existing.scalar_one()


async def claim_due_batch(
    connection: AsyncConnection,
    *,
    worker_id: str,
    lease_seconds: int,
    batch_size: int,
) -> list[OutboxEvent]:
    """Claim pending rows and expired leases in one short DB transaction."""
    selected = await connection.execute(
        text(
            """
            SELECT id
            FROM outbox_events
            WHERE (status = 'pending' AND available_at <= now())
               OR (status = 'leased' AND lease_until <= now())
            ORDER BY available_at, created_at, id
            FOR UPDATE SKIP LOCKED
            LIMIT :batch_size
            """
        ),
        {"batch_size": batch_size},
    )
    event_ids = list(selected.scalars())
    if not event_ids:
        return []
    leased = await connection.execute(
        text(
            """
            UPDATE outbox_events
            SET status = 'leased', lease_owner = :worker_id,
                lease_until = now() + (:lease_seconds * interval '1 second'),
                attempt_count = attempt_count + 1
            WHERE id = ANY(CAST(:event_ids AS uuid[]))
            RETURNING id, event_type, schema_version, aggregate_type, aggregate_id, payload_json,
                      idempotency_key, attempt_count, retry_count, available_at, causation_request_id
            """
        ),
        {"worker_id": worker_id, "lease_seconds": lease_seconds, "event_ids": event_ids},
    )
    return [_event_from_row(dict(row)) for row in leased.mappings()]


async def mark_done(connection: AsyncConnection, *, event_id: UUID, worker_id: str) -> bool:
    result = await connection.execute(
        text(
            """
            UPDATE outbox_events
            SET status = 'done', completed_at = now(), lease_owner = NULL, lease_until = NULL
            WHERE id = :event_id AND status = 'leased' AND lease_owner = :worker_id
            """
        ),
        {"event_id": event_id, "worker_id": worker_id},
    )
    return result.rowcount == 1


async def reschedule_or_dead_letter(
    connection: AsyncConnection,
    *,
    event_id: UUID,
    worker_id: str,
    error_code: str,
    max_attempts: int,
    backoff_seconds: int,
) -> str | None:
    """Persist failure metadata; no exception message or payload is kept in the row."""
    current = await connection.execute(
        text(
            """
            SELECT retry_count
            FROM outbox_events
            WHERE id = :event_id AND status = 'leased' AND lease_owner = :worker_id
            FOR UPDATE
            """
        ),
        {"event_id": event_id, "worker_id": worker_id},
    )
    retry_count = current.scalar_one_or_none()
    if retry_count is None:
        return None
    retry_count = int(retry_count) + 1
    next_status = "dead" if retry_count >= max_attempts else "pending"
    await connection.execute(
        text(
            """
            UPDATE outbox_events
            SET status = CAST(:next_status AS outbox_status),
                available_at = CASE WHEN CAST(:next_status AS outbox_status) = 'pending'
                  THEN now() + (:backoff_seconds * interval '1 second') ELSE available_at END,
                lease_owner = NULL, lease_until = NULL,
                retry_count = :retry_count,
                last_error_code = :error_code, last_error_at = now(),
                dead_at = CASE WHEN CAST(:next_status AS outbox_status) = 'dead' THEN now() ELSE dead_at END
            WHERE id = :event_id
            """
        ),
        {
            "event_id": event_id,
            "next_status": next_status,
            "backoff_seconds": backoff_seconds,
            "error_code": error_code,
            "retry_count": retry_count,
        },
    )
    return next_status


async def defer_same_event(
    connection: AsyncConnection, *, event_id: UUID, worker_id: str, delay_seconds: int
) -> bool:
    """Put a successfully handled long-running poll back to pending; no retry/dead-letter budget spent."""
    result = await connection.execute(
        text(
            """
            UPDATE outbox_events
            SET status = 'pending', available_at = now() + (:delay_seconds * interval '1 second'),
                lease_owner = NULL, lease_until = NULL
            WHERE id = :event_id AND status = 'leased' AND lease_owner = :worker_id
            """
        ),
        {"event_id": event_id, "worker_id": worker_id, "delay_seconds": delay_seconds},
    )
    return result.rowcount == 1


async def release_expired_leases(connection: AsyncConnection) -> int:
    """Operational sweep; claim also sees expired leases directly, so this is optional hygiene."""
    result = await connection.execute(
        text(
            """
            UPDATE outbox_events
            SET status = 'pending', lease_owner = NULL, lease_until = NULL
            WHERE status = 'leased' AND lease_until <= now()
            """
        )
    )
    return result.rowcount or 0
