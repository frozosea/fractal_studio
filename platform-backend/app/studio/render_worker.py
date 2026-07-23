from __future__ import annotations

import uuid
from datetime import datetime, timedelta, timezone

from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from app.core.config import Settings
from app.infrastructure.compute.compute_client import ComputeClient, ComputeError
from app.outbox.service import append_event

from .models import QuotaReservation, RenderJob


TERMINAL = {"completed", "failed", "cancelled"}


class RenderWorker:
    def __init__(self, settings: Settings, compute: ComputeClient) -> None:
        self._settings = settings
        self._compute = compute

    async def _locked_job(self, session: AsyncSession, job_id: uuid.UUID) -> RenderJob | None:
        return await session.scalar(
            select(RenderJob).where(RenderJob.id == job_id).with_for_update()
        )

    async def submit(self, session: AsyncSession, job_id: uuid.UUID) -> None:
        job = await self._locked_job(session, job_id)
        if job is None or job.status in TERMINAL or job.compute_run_id:
            return
        result = await self._compute.create_run(
            job.kind, dict(job.request_json), str(job.id), f"worker:{job.id}"
        )
        job.compute_node_id = self._settings.compute_node_id
        job.compute_run_id = str(result["computeRunId"])
        job.status = str(result.get("status", "running"))
        job.updated_at = datetime.now(timezone.utc)
        if job.status == "completed":
            await self._complete(session, job)
            return
        await self._schedule_poll(session, job, sequence=0)

    async def poll(self, session: AsyncSession, job_id: uuid.UUID, sequence: int) -> None:
        job = await self._locked_job(session, job_id)
        if job is None or job.status in TERMINAL:
            return
        if not job.compute_run_id:
            raise RuntimeError("render job has no compute_run_id")
        result = await self._compute.get_run(job.compute_run_id)
        compute_status = str(result.get("status", "running"))
        progress = result.get("progress") or {}
        percent = progress.get("percent", job.progress_percent)
        if isinstance(percent, (int, float)):
            job.progress_percent = max(0, min(100, int(percent)))
        job.updated_at = datetime.now(timezone.utc)
        if compute_status == "completed":
            await self._complete(session, job)
        elif compute_status in {"failed", "cancelled"}:
            job.status = compute_status
            job.finished_at = datetime.now(timezone.utc)
            await self._release_quota(session, job.id, "released")
        else:
            job.status = "running"
            await self._schedule_poll(session, job, sequence=sequence + 1)

    async def cancel(self, session: AsyncSession, job_id: uuid.UUID) -> None:
        job = await self._locked_job(session, job_id)
        if job is None or job.status in TERMINAL:
            return
        if not job.compute_run_id:
            job.status = "cancelled"
            job.finished_at = datetime.now(timezone.utc)
            await self._release_quota(session, job.id, "released")
            return
        result = await self._compute.cancel_run(job.compute_run_id)
        if result.get("status") == "cancelled":
            job.status = "cancelled"
            job.finished_at = datetime.now(timezone.utc)
            await self._release_quota(session, job.id, "released")
        else:
            await self._schedule_poll(session, job, sequence=0)

    async def _complete(self, session: AsyncSession, job: RenderJob) -> None:
        if not job.compute_run_id:
            raise RuntimeError("completed render missing compute_run_id")
        manifest = await self._compute.manifest(job.compute_run_id)
        if manifest.get("status") != "completed":
            raise ComputeError("INVALID_MANIFEST", "completed Compute run returned non-completed manifest")
        for artifact in manifest.get("artifacts", []):
            if (
                not artifact.get("artifactId") or not artifact.get("name")
                or not artifact.get("sha256") or artifact.get("sizeBytes") is None
            ):
                raise ComputeError("INVALID_MANIFEST", "artifact is missing id, name, size or SHA-256")
            name = str(artifact["name"])
            if name.startswith("/") or ".." in name.split("/"):
                raise ComputeError("INVALID_MANIFEST", "artifact name is not a safe relative path")
            size_bytes = artifact["sizeBytes"]
            if not isinstance(size_bytes, int) or isinstance(size_bytes, bool) or size_bytes < 0:
                raise ComputeError("INVALID_MANIFEST", "artifact sizeBytes must be a non-negative integer")
            sha256 = str(artifact["sha256"])
            if len(sha256) != 64 or any(character not in "0123456789abcdefABCDEF" for character in sha256):
                raise ComputeError("INVALID_MANIFEST", "artifact SHA-256 must contain 64 hexadecimal characters")
            await self._compute.verify_artifact(str(artifact["artifactId"]), size_bytes, sha256)
        job.result_manifest_json = manifest
        job.status = "completed"
        job.progress_percent = 100
        job.finished_at = datetime.now(timezone.utc)
        job.updated_at = job.finished_at
        await self._release_quota(session, job.id, "consumed")

    async def _release_quota(self, session: AsyncSession, job_id: uuid.UUID, status: str) -> None:
        reservation = await session.scalar(
            select(QuotaReservation).where(QuotaReservation.render_job_id == job_id).with_for_update()
        )
        if reservation is not None and reservation.status == "reserved":
            reservation.status = status

    async def _schedule_poll(self, session: AsyncSession, job: RenderJob, sequence: int) -> None:
        await append_event(
            session,
            event_type="render.poll",
            aggregate_type="render_job",
            aggregate_id=job.id,
            payload={"renderJobId": str(job.id), "sequence": sequence},
            idempotency_key=f"render.poll:{job.id}:{sequence}",
            available_at=datetime.now(timezone.utc) + timedelta(seconds=1),
        )
