"""M2 durable Compute work invoked only by claimed M7 outbox events."""

from __future__ import annotations

from uuid import UUID

from app.assets.ports import AssetIngestionPort
from app.core.config import Settings, get_settings
from app.core.db import get_engine
from app.infrastructure.compute.compute_client import ComputeArtifact, ComputeClient, ComputeClientError
from app.outbox.models import NewOutboxEvent, OutboxEvent, RescheduleOutboxEvent, RetryableOutboxError
from app.outbox.service import TransactionalOutboxService
from app.outbox.worker import HandlerRegistry
from app.studio import render_job_repository


class DeferredAssetIngestionPort:
    """T06 preserves completed metadata; T07 replaces this with durable M3 ingestion."""

    async def create_from_completed_render(self, *, render_job_id: UUID) -> None:
        raise RescheduleOutboxEvent(delay_seconds=30)


class RenderWorker:
    def __init__(
        self,
        *,
        compute_client: ComputeClient | None = None,
        ingestion_port: AssetIngestionPort | None = None,
        settings: Settings | None = None,
    ) -> None:
        self._settings = settings or get_settings()
        self._compute = compute_client or ComputeClient(self._settings)
        self._ingestion = ingestion_port or DeferredAssetIngestionPort()

    @staticmethod
    def _job_id(event: OutboxEvent) -> UUID:
        try:
            job_id = UUID(str(event.payload["renderJobId"]))
        except (KeyError, TypeError, ValueError) as error:
            raise RetryableOutboxError("invalid_render_event") from error
        if job_id != event.aggregate_id:
            raise RetryableOutboxError("render_event_aggregate_mismatch")
        return job_id

    async def submit_image_video_or_mesh_run(self, event: OutboxEvent) -> None:
        """Claim state in DB, submit exact persisted DTO outside lock, then save returned run ID."""
        job_id = self._job_id(event)
        async with get_engine().begin() as connection:
            job = await render_job_repository.lock_for_worker(connection, job_id=job_id)
            if job is None or job.status in render_job_repository.TERMINAL_STATUSES:
                return
            if job.status == "cancel_requested":
                await render_job_repository.cancel_and_release(connection, job_id=job_id)
                return
            if job.status not in {"queued", "submitting"}:
                return
            if job.status == "queued":
                await render_job_repository.set_status(
                    connection, job_id=job_id, expected={"queued"}, next_status="submitting"
                )
            request = job.compute_request
        try:
            route = request["route"]
            body = request["body"]
            if not isinstance(route, str) or not isinstance(body, dict):
                raise ValueError("invalid_saved_compute_request")
            run = await self._compute.create_durable_run(route=route, request_body=body)
        except ComputeClientError as error:
            if error.code in {"compute_timeout", "compute_unavailable", "compute_conflict"}:
                raise RetryableOutboxError(error.code) from error
            await self._fail(job_id, error.code)
            return
        except (KeyError, TypeError, ValueError):
            await self._fail(job_id, "invalid_saved_compute_request")
            return
        if run.client_job_id != str(job_id):
            await self._fail(job_id, "compute_client_job_mismatch")
            return
        async with get_engine().begin() as connection:
            current = await render_job_repository.lock_for_worker(connection, job_id=job_id)
            if current is None:
                return
            if current.status == "submitting":
                saved = await render_job_repository.save_submission(connection, job_id=job_id, run_id=run.run_id)
                if saved is not None:
                    await TransactionalOutboxService(connection).append(
                        NewOutboxEvent(
                            event_type="render.poll.v1",
                            aggregate_type="render_job",
                            aggregate_id=job_id,
                            idempotency_key="poll",
                            payload={"renderJobId": str(job_id)},
                            causation_request_id=event.causation_request_id,
                        )
                    )
            elif current.status == "cancel_requested":
                await render_job_repository.save_run_id_if_cancelling(connection, job_id=job_id, run_id=run.run_id)

    async def poll_run_status(self, event: OutboxEvent) -> None:
        job_id = self._job_id(event)
        async with get_engine().connect() as connection:
            job = await render_job_repository.lock_for_worker(connection, job_id=job_id)
        if job is None or job.status in render_job_repository.TERMINAL_STATUSES:
            return
        if job.status == "compute_succeeded":
            await self._ingestion.create_from_completed_render(render_job_id=job_id)
            return
        if job.status == "cancel_requested":
            raise RescheduleOutboxEvent(delay_seconds=1)
        if job.status != "running" or not job.compute_run_id:
            raise RetryableOutboxError("render_job_not_pollable")
        try:
            run = await self._compute.get_run_status(run_id=job.compute_run_id)
        except ComputeClientError as error:
            raise RetryableOutboxError(error.code) from error
        if run.client_job_id != str(job_id):
            await self._fail(job_id, "compute_client_job_mismatch")
            return
        if run.status in {"queued", "running", "submitting"}:
            async with get_engine().begin() as connection:
                await render_job_repository.update_progress(
                    connection, job_id=job_id, progress_percent=max(0, min(100, run.progress_percent))
                )
            raise RescheduleOutboxEvent(delay_seconds=self._settings.render_poll_interval_seconds)
        if run.status == "cancelled":
            async with get_engine().begin() as connection:
                await render_job_repository.cancel_and_release(connection, job_id=job_id)
            return
        if run.status == "failed":
            await self._fail(job_id, run.error_code or "compute_failed")
            return
        if run.status != "completed":
            await self._fail(job_id, "compute_invalid_status")
            return
        selected = self._select_artifacts(job.output_spec, run.artifacts)
        if not selected:
            await self._fail(job_id, "compute_master_artifact_missing")
            return
        metadata = {
            "runId": run.run_id,
            "status": run.status,
            "artifacts": [
                {
                    "artifactId": item.artifact_id,
                    "purpose": item.purpose,
                    "mediaType": item.media_type,
                    "sizeBytes": item.size_bytes,
                }
                for item in selected
            ],
        }
        async with get_engine().begin() as connection:
            saved = await render_job_repository.save_compute_success(
                connection,
                job_id=job_id,
                result_metadata=metadata,
                selected_artifact_ids=[item.artifact_id for item in selected],
            )
        if saved:
            await self._ingestion.create_from_completed_render(render_job_id=job_id)

    async def forward_cancellation(self, event: OutboxEvent) -> None:
        job_id = self._job_id(event)
        async with get_engine().connect() as connection:
            job = await render_job_repository.lock_for_worker(connection, job_id=job_id)
        if job is None or job.status in render_job_repository.TERMINAL_STATUSES:
            return
        if job.status != "cancel_requested":
            return
        if not job.compute_run_id:
            raise RescheduleOutboxEvent(delay_seconds=1)
        try:
            await self._compute.cancel_run(run_id=job.compute_run_id)
        except ComputeClientError as error:
            raise RetryableOutboxError(error.code) from error
        async with get_engine().begin() as connection:
            await render_job_repository.cancel_and_release(connection, job_id=job_id)

    async def _fail(self, job_id: UUID, error_code: str) -> None:
        async with get_engine().begin() as connection:
            await render_job_repository.fail_and_release(connection, job_id=job_id, error_code=error_code)

    @staticmethod
    def _select_artifacts(
        output_spec: dict[str, object], artifacts: tuple[ComputeArtifact, ...]
    ) -> list[ComputeArtifact]:
        expected_media = {
            "image": "image/png",
            "video": "video/mp4",
            "hs_mesh": {"glb": "model/gltf-binary", "stl": "model/stl"},
            "transition_mesh": {"glb": "model/gltf-binary", "stl": "model/stl"},
        }
        kind = output_spec.get("kind")
        expected = expected_media.get(kind)
        if isinstance(expected, dict):
            expected = expected.get(output_spec.get("format"))
        if not isinstance(expected, str):
            return []
        return [
            artifact
            for artifact in artifacts
            if artifact.purpose == "master" and artifact.media_type == expected and 0 < artifact.size_bytes <= 524_288_000
        ][:1]


def build_render_handler_registry(
    *, ingestion_port: AssetIngestionPort | None = None, settings: Settings | None = None
) -> HandlerRegistry:
    render_worker = RenderWorker(ingestion_port=ingestion_port, settings=settings)
    registry = HandlerRegistry()
    registry.register("render.created.v1", render_worker.submit_image_video_or_mesh_run)
    registry.register("render.poll.v1", render_worker.poll_run_status)
    registry.register("render.cancel_requested.v1", render_worker.forward_cancellation)
    return registry
