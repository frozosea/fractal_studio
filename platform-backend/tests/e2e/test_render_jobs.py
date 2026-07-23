"""Live T06 API + PostgreSQL outbox + contract-compatible Compute-stub checks."""

from __future__ import annotations

import asyncio
import os
import uuid
from dataclasses import dataclass, field
from uuid import UUID

import httpx
import pytest
from sqlalchemy import text
from sqlalchemy.ext.asyncio import create_async_engine

from app.assets.ports import AssetIngestionPort
from app.core.config import Settings
from app.outbox.worker import OutboxWorker
from app.studio.render_worker import build_render_handler_registry


@dataclass
class RecordingIngestion(AssetIngestionPort):
    completed_job_ids: list[UUID] = field(default_factory=list)

    async def create_from_completed_render(self, *, render_job_id: UUID) -> None:
        self.completed_job_ids.append(render_job_id)


def _settings() -> Settings:
    database_url = os.getenv("E2E_DATABASE_URL")
    if not database_url:
        raise RuntimeError("E2E_DATABASE_URL is required")
    return Settings(
        database_url=database_url,
        session_secret="test-session-secret-long-enough",
        compute_base_url=os.getenv("E2E_COMPUTE_URL", "http://127.0.0.1:18088"),
        compute_service_key="test-compute-key",
        render_poll_interval_seconds=1,
        outbox_backoff_base_seconds=1,
    )


async def _job_db_state(job_id: str) -> tuple[str, str, int, int, int, int]:
    engine = create_async_engine(os.environ["E2E_DATABASE_URL"])
    try:
        async with engine.connect() as connection:
            result = await connection.execute(
                text(
                    """
                    SELECT j.status::text, COALESCE(j.compute_run_id, ''),
                      (SELECT count(*) FROM quota_reservations q WHERE q.render_job_id = j.id),
                      (SELECT count(*) FROM outbox_events o WHERE o.aggregate_id = j.id AND o.event_type = 'render.created.v1'),
                      (SELECT count(*) FROM outbox_events o WHERE o.aggregate_id = j.id AND o.event_type = 'render.poll.v1'),
                      (SELECT count(*) FROM quota_reservations q WHERE q.render_job_id = j.id AND q.status = 'released')
                    FROM render_jobs j WHERE j.id = CAST(:job_id AS uuid)
                    """
                ),
                {"job_id": job_id},
            )
            row = result.one()
        return tuple(row)  # type: ignore[return-value]
    finally:
        await engine.dispose()


async def _run_worker_to_compute_success() -> RecordingIngestion:
    ingestion = RecordingIngestion()
    settings = _settings()
    worker = OutboxWorker(
        worker_id=f"t06-{uuid.uuid4()}",
        handlers=build_render_handler_registry(ingestion_port=ingestion, settings=settings),
        settings=settings,
    )
    await worker.poll_once()  # render.created.v1 -> Compute submit -> same poll row created
    await worker.poll_once()  # poll -> Compute running -> same event deferred
    await asyncio.sleep(1.05)
    await worker.poll_once()  # same poll row -> Compute completed -> M3 port
    return ingestion


async def _run_worker_after_one_transient_submit_failure() -> RecordingIngestion:
    ingestion = RecordingIngestion()
    settings = _settings()
    worker = OutboxWorker(
        worker_id=f"t06-transient-{uuid.uuid4()}",
        handlers=build_render_handler_registry(ingestion_port=ingestion, settings=settings),
        settings=settings,
    )
    await worker.poll_once()  # transient 503: event is retry-scheduled, job remains submitting
    await asyncio.sleep(1.05)
    await worker.poll_once()  # same render.created.v1 -> submits exact persisted DTO
    await worker.poll_once()  # poll -> running
    await asyncio.sleep(1.05)
    await worker.poll_once()  # same poll row -> completed
    return ingestion


@pytest.mark.skipif(
    not (os.getenv("E2E_API_URL") and os.getenv("E2E_DATABASE_URL") and os.getenv("E2E_COMPUTE_AVAILABLE") == "1"),
    reason="set E2E_API_URL, E2E_DATABASE_URL and E2E_COMPUTE_AVAILABLE=1",
)
@pytest.mark.asyncio
async def test_durable_render_job_lifecycle_and_cancel(e2e_api_url: str) -> None:
    suffix = uuid.uuid4().hex[:10]
    with httpx.Client(base_url=e2e_api_url, timeout=10, trust_env=False) as client:
        assert client.post(
            "/v1/auth/register",
            json={"email": f"render-{suffix}@example.test", "password": "correct-horse-01"},
        ).status_code == 201
        recipe = client.post(
            "/v1/recipes",
            headers={"Idempotency-Key": "recipe"},
            json={"canonicalSpec": {"version": 1, "seed": 42}},
        )
        assert recipe.status_code == 201
        recipe_id = recipe.json()["data"]["id"]

        created = client.post(
            "/v1/render-jobs",
            headers={"Idempotency-Key": "job-image"},
            json={"recipeId": recipe_id, "output": {"kind": "image", "format": "png", "width": 64, "height": 64}},
        )
        assert created.status_code == 202
        job_id = created.json()["data"]["id"]
        assert created.json()["data"]["status"] == "queued"
        assert await _job_db_state(job_id) == ("queued", "", 1, 1, 0, 0)

        replay = client.post(
            "/v1/render-jobs",
            headers={"Idempotency-Key": "job-image"},
            json={"recipeId": recipe_id, "output": {"kind": "image", "format": "png", "width": 64, "height": 64}},
        )
        assert replay.status_code == 202
        assert replay.json()["data"]["id"] == job_id

        ingestion = await _run_worker_to_compute_success()
        assert ingestion.completed_job_ids == [UUID(job_id)]

        completed = client.get(f"/v1/render-jobs/{job_id}")
        assert completed.status_code == 200
        assert completed.json()["data"]["status"] == "compute_succeeded"
        assert completed.json()["data"]["progressPercent"] == 100
        job_state = await _job_db_state(job_id)
        assert job_state[0] == "compute_succeeded"
        assert job_state[1].startswith("run-")
        assert job_state[2:] == (1, 1, 1, 0)  # one reservation, one created, one reused poll row

        cannot_cancel = client.post(
            f"/v1/render-jobs/{job_id}/cancel", headers={"Idempotency-Key": "cancel-complete"}
        )
        assert cannot_cancel.status_code == 409

        queued = client.post(
            "/v1/render-jobs",
            headers={"Idempotency-Key": "job-cancel"},
            json={"recipeId": recipe_id, "output": {"kind": "image", "format": "png", "width": 32, "height": 32}},
        )
        assert queued.status_code == 202
        cancel_job_id = queued.json()["data"]["id"]
        cancelled = client.post(
            f"/v1/render-jobs/{cancel_job_id}/cancel", headers={"Idempotency-Key": "cancel-before-submit"}
        )
        assert cancelled.status_code == 202
        await _run_worker_to_compute_success()
        final = client.get(f"/v1/render-jobs/{cancel_job_id}")
        assert final.status_code == 200
        assert final.json()["data"]["status"] == "cancelled"
        assert (await _job_db_state(cancel_job_id))[5] == 1

        invalid = client.post(
            "/v1/render-jobs",
            headers={"Idempotency-Key": "job-invalid"},
            json={"recipeId": recipe_id, "output": {"kind": "image", "format": "jpg", "width": 64, "height": 64}},
        )
        assert invalid.status_code == 422

        transient_recipe = client.post(
            "/v1/recipes",
            headers={"Idempotency-Key": "recipe-transient"},
            json={"canonicalSpec": {"version": 1, "variant": "transient_failure"}},
        )
        assert transient_recipe.status_code == 201
        transient_job = client.post(
            "/v1/render-jobs",
            headers={"Idempotency-Key": "job-transient"},
            json={
                "recipeId": transient_recipe.json()["data"]["id"],
                "output": {"kind": "image", "format": "png", "width": 16, "height": 16},
            },
        )
        assert transient_job.status_code == 202
        transient_job_id = transient_job.json()["data"]["id"]
        transient_ingestion = await _run_worker_after_one_transient_submit_failure()
        assert transient_ingestion.completed_job_ids == [UUID(transient_job_id)]
        assert (await _job_db_state(transient_job_id))[0] == "compute_succeeded"
