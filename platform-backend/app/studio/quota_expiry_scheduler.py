"""M2-owned due-work reader for stale durable render reservations."""

from __future__ import annotations

from app.outbox.models import NewOutboxEvent
from app.outbox.service import TransactionalOutboxService
from app.studio import render_job_repository


class RenderQuotaExpiryScheduler:
    async def schedule_due_work(self, service: TransactionalOutboxService) -> int:
        job_ids = await render_job_repository.find_expired_reservation_job_ids(service.connection)
        for job_id in job_ids:
            await service.append(
                NewOutboxEvent(
                    event_type="render.quota_expired.v1",
                    aggregate_type="render_job",
                    aggregate_id=job_id,
                    idempotency_key="quota-expired",
                    payload={"renderJobId": str(job_id)},
                )
            )
        return len(job_ids)
