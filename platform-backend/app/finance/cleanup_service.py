"""M6b private QR retention. Worker retries storage failures through its outbox lease."""

from __future__ import annotations

from datetime import UTC, datetime, timedelta
from uuid import UUID

from app.core import audit_writer
from app.core.config import Settings, get_settings
from app.core.db import get_engine
from app.finance import repository
from app.infrastructure.storage.object_storage import ObjectStorage
from app.outbox.models import NewOutboxEvent, OutboxEvent, RetryableOutboxError
from app.outbox.service import TransactionalOutboxService


async def queue_terminal_qr_cleanup(
    connection, *, payout_request_id: UUID, status: str, request_id_value: str
) -> None:
    settings = get_settings()
    if status not in {"paid", "rejected", "cancelled"}:
        raise ValueError("invalid_payout_cleanup_status")
    days = settings.payout_qr_paid_retention_days if status == "paid" else settings.payout_qr_rejected_retention_days
    await TransactionalOutboxService(connection).append(
        NewOutboxEvent(
            event_type="payout.qr_cleanup.v1", aggregate_type="payout_request", aggregate_id=payout_request_id,
            idempotency_key=f"terminal:{status}", payload={"payoutRequestId": str(payout_request_id)},
            available_at=datetime.now(UTC) + timedelta(days=days), causation_request_id=request_id_value,
        )
    )


class PayoutQrCleanupService:
    def __init__(self, *, storage: ObjectStorage | None = None, settings: Settings | None = None) -> None:
        self._storage = storage or ObjectStorage()
        self._settings = settings or get_settings()

    async def cleanup_qr(self, event: OutboxEvent) -> None:
        try:
            payout_id = UUID(str(event.payload["payoutRequestId"]))
        except (KeyError, TypeError, ValueError) as error:
            raise RetryableOutboxError("invalid_payout_qr_cleanup_event") from error
        if payout_id != event.aggregate_id:
            raise RetryableOutboxError("payout_qr_cleanup_aggregate_mismatch")
        async with get_engine().begin() as connection:
            payout = await repository.lock_payout_request(connection, payout_request_id=payout_id)
            if payout is None or payout.qr_deleted_at is not None:
                return
            if payout.status not in {"paid", "rejected", "cancelled"}:
                return
            object_key = payout.qr_object_key
        try:
            await self._storage.delete(object_key=object_key)
        except Exception as error:
            raise RetryableOutboxError("payout_qr_cleanup_failed") from error
        async with get_engine().begin() as connection:
            await repository.mark_qr_deleted(connection, payout_request_id=payout_id)

    async def dead_letter(self, event: OutboxEvent, error_code: str) -> None:
        """Never silently abandon private QR retention after generic retry exhaustion."""
        try:
            payout_id = UUID(str(event.payload["payoutRequestId"]))
        except (KeyError, TypeError, ValueError):
            return
        if payout_id != event.aggregate_id:
            return
        async with get_engine().begin() as connection:
            payout = await repository.lock_payout_request(connection, payout_request_id=payout_id)
            if payout is None or payout.qr_deleted_at is not None:
                return
            if payout.status not in {"paid", "rejected", "cancelled"}:
                return
            await TransactionalOutboxService(connection).append(
                NewOutboxEvent(
                    event_type="payout.qr_cleanup.v1",
                    aggregate_type="payout_request",
                    aggregate_id=payout_id,
                    idempotency_key=f"dead-retry:{event.id}",
                    payload={"payoutRequestId": str(payout_id)},
                    available_at=datetime.now(UTC)
                    + timedelta(seconds=self._settings.payout_qr_cleanup_retry_delay_seconds),
                    causation_request_id=event.causation_request_id,
                )
            )
            await audit_writer.record_system_action(
                connection,
                action="payout.qr_cleanup_dead_lettered",
                subject_type="payout_request",
                subject_id=payout_id,
                request_id_value=event.causation_request_id or f"outbox:{event.id}",
                metadata={"errorCode": error_code, "eventId": str(event.id)},
            )
