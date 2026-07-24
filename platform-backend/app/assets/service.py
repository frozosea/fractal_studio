"""M3 verified Compute artifact ingestion. Browser never reaches this boundary."""

from __future__ import annotations

import hashlib
import json
from datetime import UTC, datetime, timedelta
from pathlib import Path
from typing import Any
from uuid import UUID, uuid4

from fastapi import HTTPException, Request, status

from app.assets import repository
from app.assets.cleanup_service import queue_object_cleanup
from app.assets.models import AssetFileView, AssetView, DownloadUrlView
from app.assets.ports import EntitlementReader
from app.auth.models import AccessPrincipal
from app.core import audit_writer
from app.core.config import get_settings
from app.core.db import get_engine
from app.core import idempotency_service
from app.core.request_context import request_id
from app.commerce.entitlement_reader import PostgresEntitlementReader
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
            await queue_object_cleanup(connection, object_keys=object_keys)

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


def asset_view(record: repository.AssetRecord) -> AssetView:
    """Single safe mapping: neither storage keys nor Compute provenance leave M3."""
    return AssetView(
        id=record.id,
        recipeId=record.recipe_id,
        mediaType=record.media_type,
        status=record.status,
        visibility=record.visibility,
        derivativeStatus=record.derivative_status,
        derivativeErrorCode=record.derivative_error_code,
        createdAt=record.created_at,
        files=[
            AssetFileView.model_validate(item)
            for item in record.files
            if item.get("purpose") != "render_manifest"
        ],
    )


class AssetLibraryService:
    def __init__(
        self,
        *,
        object_storage: ObjectStorage | None = None,
        entitlement_reader: EntitlementReader | None = None,
    ) -> None:
        self._storage = object_storage or ObjectStorage()
        self._entitlements = entitlement_reader or PostgresEntitlementReader()

    async def change_visibility(
        self,
        *,
        principal: AccessPrincipal,
        asset_id: UUID,
        visibility: str,
        idempotency_key: str,
        request: Request,
    ) -> tuple[dict[str, object], int, dict[str, str]]:
        async with get_engine().begin() as connection:
            claim = await idempotency_service.claim(
                connection,
                user_id=principal.user_id,
                scope="assets.visibility",
                key=idempotency_key,
                body={"assetId": str(asset_id), "visibility": visibility},
            )
            if claim.is_replay:
                return claim.replay_body or {}, claim.replay_status or 200, claim.replay_headers or {}
            result = await repository.change_visibility(
                connection,
                asset_id=asset_id,
                owner_id=principal.user_id,
                visibility=visibility,
            )
            self._raise_mutation_error(result.code)
            assert result.asset is not None
            body: dict[str, object] = {"data": asset_view(result.asset).model_dump(mode="json", by_alias=True)}
            headers = {"Cache-Control": "no-store"}
            await audit_writer.record_user_action(
                connection,
                actor_user_id=principal.user_id,
                action="asset.visibility_changed",
                subject_type="asset",
                subject_id=asset_id,
                request_id_value=request_id(request),
                metadata={"visibility": visibility},
            )
            await idempotency_service.complete(
                connection,
                claim,
                response_status=status.HTTP_200_OK,
                response_body=body,
                response_headers=headers,
            )
        return body, status.HTTP_200_OK, headers

    async def soft_delete(
        self,
        *,
        principal: AccessPrincipal,
        asset_id: UUID,
        idempotency_key: str,
        request: Request,
    ) -> tuple[int, dict[str, str]]:
        async with get_engine().begin() as connection:
            claim = await idempotency_service.claim(
                connection,
                user_id=principal.user_id,
                scope="assets.delete",
                key=idempotency_key,
                body={"assetId": str(asset_id)},
            )
            if claim.is_replay:
                return claim.replay_status or status.HTTP_204_NO_CONTENT, claim.replay_headers or {}
            result = await repository.soft_delete(connection, asset_id=asset_id, owner_id=principal.user_id)
            self._raise_mutation_error(result.code)
            await queue_object_cleanup(
                connection,
                object_keys=result.cleanup_keys,
                causation_request_id=request_id(request),
            )
            await audit_writer.record_user_action(
                connection,
                actor_user_id=principal.user_id,
                action="asset.soft_deleted",
                subject_type="asset",
                subject_id=asset_id,
                request_id_value=request_id(request),
                metadata={"masterRetained": not bool(result.cleanup_keys)},
            )
            headers = {"Cache-Control": "no-store"}
            await idempotency_service.complete(
                connection,
                claim,
                response_status=status.HTTP_204_NO_CONTENT,
                response_body=None,
                response_headers=headers,
            )
        return status.HTTP_204_NO_CONTENT, headers

    async def create_download_url(self, *, principal: AccessPrincipal, asset_id: UUID) -> DownloadUrlView:
        async with get_engine().connect() as connection:
            asset = await repository.find_download_asset(connection, asset_id=asset_id)
        if asset is None or asset.master_object_key is None:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="asset_not_found")
        if asset.owner_id != principal.user_id and not await self._entitlements.has_active_entitlement(
            user_id=principal.user_id, asset_id=asset_id
        ):
            raise HTTPException(status_code=status.HTTP_403_FORBIDDEN, detail="forbidden")
        settings = get_settings()
        url = await self._storage.create_signed_get_url(
            object_key=asset.master_object_key, expires_seconds=settings.master_download_ttl_seconds
        )
        return DownloadUrlView(
            url=url, expiresAt=datetime.now(UTC) + timedelta(seconds=settings.master_download_ttl_seconds)
        )

    @staticmethod
    def _raise_mutation_error(code: str) -> None:
        if code == "ok":
            return
        if code == "not_found":
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="asset_not_found")
        if code in {"listed", "deleted", "invalid_state"}:
            raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail="invalid_state")
        raise RuntimeError(f"unknown asset mutation result: {code}")
