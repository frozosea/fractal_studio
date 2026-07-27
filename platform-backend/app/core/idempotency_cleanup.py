"""Bounded lifecycle maintenance for expired browser idempotency records."""

from app.core import idempotency_repository
from app.outbox.models import OutboxService
from app.outbox.service import TransactionalOutboxService


class IdempotencyExpiryScheduler:
    async def schedule_due_work(self, service: OutboxService) -> int:
        if not isinstance(service, TransactionalOutboxService):
            raise TypeError("idempotency cleanup requires a transaction-bound outbox service")
        return await idempotency_repository.delete_expired(service.connection)
