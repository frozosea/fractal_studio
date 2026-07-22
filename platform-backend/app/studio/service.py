from __future__ import annotations

import uuid
from datetime import datetime, timedelta, timezone

from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from app.outbox.service import append_event

from .models import QuotaReservation, RenderJob


TERMINAL_STATUSES = {"completed", "failed", "cancelled"}


class RenderJobService:
    async def create_job(
        self,
        session: AsyncSession,
        *,
        owner_id: uuid.UUID,
        kind: str,
        payload: dict,
        idempotency_key: str,
    ) -> RenderJob:
        existing = await session.scalar(select(RenderJob).where(
            RenderJob.owner_id == owner_id,
            RenderJob.idempotency_key == idempotency_key,
        ))
        if existing is not None:
            return existing
        job = RenderJob(
            owner_id=owner_id,
            kind=kind,
            request_json=payload,
            idempotency_key=idempotency_key,
        )
        session.add(job)
        await session.flush()
        session.add(QuotaReservation(
            user_id=owner_id,
            render_job_id=job.id,
            expires_at=datetime.now(timezone.utc) + timedelta(hours=24),
        ))
        await append_event(
            session,
            event_type="render.created",
            aggregate_type="render_job",
            aggregate_id=job.id,
            payload={"renderJobId": str(job.id)},
            idempotency_key=f"render.created:{job.id}",
        )
        return job

    async def get_owned_job(
        self, session: AsyncSession, *, owner_id: uuid.UUID, job_id: uuid.UUID, lock: bool = False
    ) -> RenderJob | None:
        statement = select(RenderJob).where(RenderJob.id == job_id, RenderJob.owner_id == owner_id)
        if lock:
            statement = statement.with_for_update()
        return await session.scalar(statement)

    async def request_cancel(
        self, session: AsyncSession, *, owner_id: uuid.UUID, job_id: uuid.UUID
    ) -> RenderJob | None:
        job = await self.get_owned_job(session, owner_id=owner_id, job_id=job_id, lock=True)
        if job is None or job.status in TERMINAL_STATUSES:
            return job
        if job.cancel_requested_at is None:
            job.cancel_requested_at = datetime.now(timezone.utc)
            job.updated_at = job.cancel_requested_at
            await append_event(
                session,
                event_type="render.cancel_requested",
                aggregate_type="render_job",
                aggregate_id=job.id,
                payload={"renderJobId": str(job.id)},
                idempotency_key=f"render.cancel_requested:{job.id}",
            )
        return job

