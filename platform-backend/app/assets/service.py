"""M3 verified Compute artifact ingestion. Browser never reaches this boundary."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
from typing import Any
from uuid import UUID, uuid4

from app.assets import repository
from app.core import audit_writer
from app.core.db import get_engine
from app.infrastructure.compute.compute_artifact_reader import ArtifactIntegrityError, ComputeArtifactReader
from app.infrastructure.storage.object_storage import ObjectStorage
from app.outbox.models import NewOutboxEvent
from app.outbox.service import TransactionalOutboxService
from app.studio import render_job_repository


_EXTENSION_BY_MEDIA_TYPE = {
    "image/png": ".png",
    "video/mp4": ".mp4",
    "model/gltf-binary": ".glb",
    "application/sla": ".stl",
}
_MAX_ARTIFACT_SIZE = 524_288_000


class AssetIngestionService:
    def __init__(
        self,
        *,
        artifact_reader: ComputeArtifactReader | None = None,
        object_storage: ObjectStorage | None = None,
    ) -> None:
        self._artifact_reader = artifact_reader or ComputeArtifactReader()
        self._storage = object_storage or ObjectStorage()

    async def create_from_completed_render(self, *, render_job_id: UUID) -> None:
        """Commit processing row, do I/O unlocked, then atomically finalize product state."""
        asset_id: UUID | None = None
        uploaded_keys: list[str] = []
        temporary_file: Path | None = None
        try:
            async with get_engine().begin() as connection:
                job = await render_job_repository.lock_for_worker(connection, job_id=render_job_id)
                if job is None or job.status == "completed":
                    return
                if job.status != "compute_succeeded":
                    raise ArtifactIntegrityError("render_not_ready_for_ingest")
                asset_id = await repository.create_or_get_processing(
                    connection,
                    owner_id=job.owner_id,
                    recipe_id=job.recipe_id,
                    render_job_id=job.id,
                    media_type=self._asset_media_type(job.output_kind),
                )
                if await repository.get_asset_status(connection, asset_id=asset_id) == "ready":
                    await self._complete_job(connection, job_id=job.id, asset_id=asset_id)
                    return
                artifact = self._master_metadata(job.compute_result, job.selected_artifact_ids)
                provenance = self._provenance(job, artifact)
                await render_job_repository.set_status(
                    connection,
                    job_id=job.id,
                    expected={"compute_succeeded"},
                    next_status="ingesting",
                )

            # No DB lock during Compute stream or S3 calls.
            verified = await self._artifact_reader.download_verified(
                artifact_id=artifact["artifactId"],
                expected_sha256=artifact["sha256"],
                expected_size_bytes=artifact["sizeBytes"],
            )
            temporary_file = verified.path
            verified_sha256 = verified.sha256
            verified_size = verified.size_bytes
            if verified_size != artifact["sizeBytes"] or verified_sha256 != artifact["sha256"]:
                raise ArtifactIntegrityError("artifact_checksum_mismatch")

            master_file_id = uuid4()
            master_key = self._master_object_key(asset_id, master_file_id, artifact["filename"])
            await self._storage.upload_file(
                object_key=master_key, source=temporary_file, media_type=artifact["mediaType"]
            )
            uploaded_keys.append(master_key)

            manifest_file_id = uuid4()
            manifest_key = f"private/provenance/{asset_id}/{manifest_file_id}/manifest.json"
            manifest_body = json.dumps(provenance, sort_keys=True, separators=(",", ":")).encode()
            await self._storage.upload_bytes(
                object_key=manifest_key, body=manifest_body, media_type="application/json"
            )
            uploaded_keys.append(manifest_key)

            async with get_engine().begin() as connection:
                job = await render_job_repository.lock_for_worker(connection, job_id=render_job_id)
                if job is None or job.status not in {"ingesting", "compute_succeeded"}:
                    raise ArtifactIntegrityError("render_job_not_ingestable")
                await repository.finalize_ingestion(
                    connection,
                    asset_id=asset_id,
                    master_file_id=master_file_id,
                    master_object_key=master_key,
                    master_sha256=verified_sha256,
                    master_size_bytes=verified_size,
                    master_media_type=artifact["mediaType"],
                    manifest_file_id=manifest_file_id,
                    manifest_object_key=manifest_key,
                    manifest_sha256=hashlib.sha256(manifest_body).hexdigest(),
                    manifest_size_bytes=len(manifest_body),
                )
                await self._complete_job(connection, job_id=render_job_id, asset_id=asset_id)
                await TransactionalOutboxService(connection).append(
                    NewOutboxEvent(
                        event_type="media.create_derivatives.v1",
                        aggregate_type="asset",
                        aggregate_id=asset_id,
                        idempotency_key="initial",
                        payload={"assetId": str(asset_id)},
                    )
                )
                await audit_writer.record_system_action(
                    connection,
                    action="asset.ingested",
                    subject_type="asset",
                    subject_id=asset_id,
                    request_id_value="-",
                    metadata={"renderJobId": str(render_job_id)},
                )
        except Exception:
            await self._mark_failure_and_queue_cleanup(
                render_job_id=render_job_id, asset_id=asset_id, object_keys=uploaded_keys
            )
            raise
        finally:
            if temporary_file is not None:
                temporary_file.unlink(missing_ok=True)

    async def _mark_failure_and_queue_cleanup(
        self, *, render_job_id: UUID, asset_id: UUID | None, object_keys: list[str]
    ) -> None:
        async with get_engine().begin() as connection:
            if asset_id is not None:
                await repository.mark_failed(connection, asset_id=asset_id)
            await render_job_repository.fail_and_release(
                connection, job_id=render_job_id, error_code="asset_ingestion_failed"
            )
            if object_keys:
                task_id = await repository.create_orphan_cleanup_task(
                    connection, object_keys=object_keys
                )
                await TransactionalOutboxService(connection).append(
                    NewOutboxEvent(
                        event_type="asset.cleanup_orphan.v1",
                        aggregate_type="storage_cleanup_task",
                        aggregate_id=task_id,
                        idempotency_key="delete",
                        payload={"cleanupTaskId": str(task_id)},
                    )
                )

    @staticmethod
    def _asset_media_type(output_kind: str) -> str:
        return {"image": "image", "video": "video", "hs_mesh": "mesh", "transition_mesh": "mesh"}[output_kind]

    @staticmethod
    def _master_metadata(
        compute_result: dict[str, object] | None, selected_ids: list[str] | None
    ) -> dict[str, Any]:
        if not compute_result or len(selected_ids or []) != 1:
            raise ArtifactIntegrityError("missing_selected_artifact")
        selected_id = selected_ids[0]
        records = compute_result.get("artifacts")
        if not isinstance(records, list):
            raise ArtifactIntegrityError("invalid_compute_artifact_metadata")
        for record in records:
            if not isinstance(record, dict) or record.get("artifactId") != selected_id:
                continue
            artifact_id = record.get("artifactId")
            media_type = record.get("mediaType")
            sha256 = record.get("sha256")
            size_bytes = record.get("sizeBytes")
            if not (
                isinstance(artifact_id, str)
                and isinstance(media_type, str)
                and isinstance(sha256, str)
                and isinstance(size_bytes, int)
            ):
                break
            _, filename = ComputeArtifactReader.validate_artifact_id(artifact_id)
            expected_extension = _EXTENSION_BY_MEDIA_TYPE.get(media_type)
            if (
                expected_extension is None
                or not filename.endswith(expected_extension)
                or not 0 < size_bytes <= _MAX_ARTIFACT_SIZE
                or len(sha256) != 64
                or any(char not in "0123456789abcdef" for char in sha256)
            ):
                raise ArtifactIntegrityError("invalid_master_artifact")
            return {
                "artifactId": artifact_id,
                "filename": filename,
                "mediaType": media_type,
                "sha256": sha256,
                "sizeBytes": size_bytes,
            }
        raise ArtifactIntegrityError("selected_artifact_not_found")

    @staticmethod
    def _provenance(job: Any, artifact: dict[str, Any]) -> dict[str, object]:
        request = job.compute_request if isinstance(job.compute_request, dict) else {}
        result = job.compute_result if isinstance(job.compute_result, dict) else {}
        return {
            "schemaVersion": 1,
            "computeRoute": request.get("route"),
            "computeRunId": result.get("runId"),
            "mapperVersion": job.mapping_version,
            "selectedArtifactIds": job.selected_artifact_ids or [],
            "calculatedChecksums": {artifact["artifactId"]: artifact["sha256"]},
        }

    @staticmethod
    def _master_object_key(asset_id: UUID, file_id: UUID, original_name: str) -> str:
        return f"private/masters/{asset_id}/{file_id}/{original_name}"

    @staticmethod
    async def _complete_job(connection: Any, *, job_id: UUID, asset_id: UUID) -> None:
        changed = await render_job_repository.set_status(
            connection, job_id=job_id, expected={"ingesting", "compute_succeeded"}, next_status="completed"
        )
        if changed:
            await render_job_repository.release_reservation(connection, job_id=job_id)
