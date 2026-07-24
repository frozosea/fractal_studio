"""M3 persistence for asset ingest; no object-store calls happen here."""

from __future__ import annotations

import json
from dataclasses import dataclass, replace
from datetime import datetime
from uuid import UUID, uuid4

from sqlalchemy import text
from sqlalchemy.ext.asyncio import AsyncConnection


@dataclass(frozen=True, slots=True)
class AssetRecord:
    id: UUID
    owner_id: UUID
    recipe_id: UUID
    media_type: str
    status: str
    visibility: str
    created_at: datetime
    files: list[dict[str, object]]
    derivative_status: str = "pending"
    derivative_error_code: str | None = None


@dataclass(frozen=True, slots=True)
class MediaSource:
    asset_id: UUID
    media_type: str
    status: str
    master_object_key: str
    master_media_type: str
    existing_purposes: frozenset[str]


@dataclass(frozen=True, slots=True)
class DownloadAsset:
    asset_id: UUID
    owner_id: UUID
    status: str
    master_object_key: str | None


@dataclass(frozen=True, slots=True)
class AssetReadRecord:
    """Internal M3 safe-reader source. Object keys never leave this repository/service pair."""

    id: UUID
    owner_id: UUID
    media_type: str
    status: str
    visibility: str
    derivative_keys: dict[str, str]


@dataclass(frozen=True, slots=True)
class MutationResult:
    asset: AssetRecord | None
    code: str
    cleanup_keys: list[str]


@dataclass(frozen=True, slots=True)
class CleanupTask:
    id: UUID
    retry_generation: int


async def create_or_get_processing(
    connection: AsyncConnection,
    *,
    owner_id: UUID,
    recipe_id: UUID,
    render_job_id: UUID,
    media_type: str,
) -> UUID:
    created = await connection.execute(
        text(
            """
            INSERT INTO assets (id, owner_id, recipe_id, render_job_id, media_type, status, visibility)
            VALUES (:id, :owner_id, :recipe_id, :render_job_id, CAST(:media_type AS media_type), 'processing', 'private')
            ON CONFLICT (render_job_id) DO NOTHING
            RETURNING id
            """
        ),
        {
            "id": uuid4(),
            "owner_id": owner_id,
            "recipe_id": recipe_id,
            "render_job_id": render_job_id,
            "media_type": media_type,
        },
    )
    asset_id = created.scalar_one_or_none()
    if asset_id is not None:
        return asset_id
    existing = await connection.scalar(
        text("SELECT id FROM assets WHERE render_job_id = :render_job_id FOR UPDATE"),
        {"render_job_id": render_job_id},
    )
    if existing is None:
        raise RuntimeError("asset_create_conflict_without_row")
    return existing


async def get_asset_status(connection: AsyncConnection, *, asset_id: UUID) -> str | None:
    return await connection.scalar(text("SELECT status::text FROM assets WHERE id = :id"), {"id": asset_id})


async def complete_master(
    connection: AsyncConnection,
    *,
    asset_id: UUID,
    master_file_id: UUID,
    object_key: str,
    sha256: str,
    size_bytes: int,
    media_type: str,
) -> None:
    await connection.execute(
        text(
            """
            INSERT INTO asset_files (id, asset_id, purpose, object_key, sha256, size_bytes, media_type)
            VALUES (:id, :asset_id, 'master', :object_key, :sha256, :size_bytes, :media_type)
            ON CONFLICT (asset_id, purpose) DO NOTHING
            """
        ),
        {
            "id": master_file_id,
            "asset_id": asset_id,
            "object_key": object_key,
            "sha256": sha256,
            "size_bytes": size_bytes,
            "media_type": media_type,
        },
    )
    await connection.execute(
        text("UPDATE assets SET status = 'ready' WHERE id = :asset_id AND status = 'processing'"),
        {"asset_id": asset_id},
    )


async def attach_render_manifest(
    connection: AsyncConnection,
    *,
    asset_id: UUID,
    manifest_file_id: UUID,
    object_key: str,
    sha256: str,
    size_bytes: int,
) -> None:
    await connection.execute(
        text(
            """
            INSERT INTO asset_files (id, asset_id, purpose, object_key, sha256, size_bytes, media_type)
            VALUES (:id, :asset_id, 'render_manifest', :object_key, :sha256, :size_bytes, 'application/json')
            ON CONFLICT (asset_id, purpose) DO NOTHING
            """
        ),
        {
            "id": manifest_file_id,
            "asset_id": asset_id,
            "object_key": object_key,
            "sha256": sha256,
            "size_bytes": size_bytes,
        },
    )


async def finalize_ingestion(
    connection: AsyncConnection,
    *,
    asset_id: UUID,
    master_file_id: UUID,
    master_object_key: str,
    master_sha256: str,
    master_size_bytes: int,
    master_media_type: str,
    manifest_file_id: UUID,
    manifest_object_key: str,
    manifest_sha256: str,
    manifest_size_bytes: int,
) -> None:
    """Write both S3-backed records and switch asset ready in one transaction."""
    await complete_master(
        connection,
        asset_id=asset_id,
        master_file_id=master_file_id,
        object_key=master_object_key,
        sha256=master_sha256,
        size_bytes=master_size_bytes,
        media_type=master_media_type,
    )
    await attach_render_manifest(
        connection,
        asset_id=asset_id,
        manifest_file_id=manifest_file_id,
        object_key=manifest_object_key,
        sha256=manifest_sha256,
        size_bytes=manifest_size_bytes,
    )


async def mark_failed(connection: AsyncConnection, *, asset_id: UUID) -> None:
    await connection.execute(
        text("UPDATE assets SET status = 'failed' WHERE id = :asset_id AND status = 'processing'"),
        {"asset_id": asset_id},
    )


async def create_orphan_cleanup_task(
    connection: AsyncConnection, *, object_keys: list[str]
) -> UUID:
    task_id = uuid4()
    await connection.execute(
        text(
            """
            INSERT INTO storage_cleanup_tasks (id, object_keys_json, status)
            VALUES (:id, CAST(:object_keys AS jsonb), 'pending')
            """
        ),
        {"id": task_id, "object_keys": json.dumps(object_keys)},
    )
    return task_id


async def lock_cleanup_task(connection: AsyncConnection, *, task_id: UUID) -> list[str] | None:
    row = await connection.execute(
        text(
            """
            SELECT object_keys_json FROM storage_cleanup_tasks
            WHERE id = :id AND status = 'pending'
            FOR UPDATE
            """
        ),
        {"id": task_id},
    )
    value = row.scalar_one_or_none()
    if not isinstance(value, list) or not all(isinstance(key, str) for key in value):
        return None
    return value


async def mark_cleanup_done(connection: AsyncConnection, *, task_id: UUID) -> None:
    await connection.execute(
        text("UPDATE storage_cleanup_tasks SET status = 'done', completed_at = now() WHERE id = :id"),
        {"id": task_id},
    )


async def mark_cleanup_retry(
    connection: AsyncConnection, *, task_id: UUID, error_code: str, delay_seconds: int
) -> None:
    await connection.execute(
        text(
            """
            UPDATE storage_cleanup_tasks
            SET available_at = now() + (:delay_seconds * interval '1 second'),
                last_error_code = :error_code, last_error_at = now()
            WHERE id = :id AND status = 'pending'
            """
        ),
        {"id": task_id, "error_code": error_code[:120], "delay_seconds": delay_seconds},
    )


async def claim_due_cleanup_tasks(connection: AsyncConnection, *, limit: int = 25) -> list[CleanupTask]:
    """Select abandoned/dead cleanup tasks; active cleanup outbox rows remain authoritative."""
    selected = await connection.execute(
        text(
            """
            SELECT c.id, c.retry_generation
            FROM storage_cleanup_tasks c
            WHERE c.status = 'pending' AND c.available_at <= now()
              AND NOT EXISTS (
                SELECT 1 FROM outbox_events e
                WHERE e.aggregate_type = 'storage_cleanup_task' AND e.aggregate_id = c.id
                  AND e.status IN ('pending', 'leased')
              )
            ORDER BY c.available_at, c.created_at, c.id
            FOR UPDATE SKIP LOCKED
            LIMIT :limit
            """
        ),
        {"limit": limit},
    )
    tasks: list[CleanupTask] = []
    for row in selected.mappings():
        task_id = row["id"]
        generation = int(row["retry_generation"]) + 1
        await connection.execute(
            text(
                """
                UPDATE storage_cleanup_tasks
                SET retry_generation = :generation,
                    available_at = now() + interval '5 minutes'
                WHERE id = :id
                """
            ),
            {"id": task_id, "generation": generation},
        )
        tasks.append(CleanupTask(id=task_id, retry_generation=generation))
    return tasks


async def find_owned(connection: AsyncConnection, *, asset_id: UUID, owner_id: UUID) -> AssetRecord | None:
    result = await connection.execute(
        text(
            """
            SELECT a.id, a.owner_id, a.recipe_id, a.media_type::text AS media_type,
                   a.status::text AS status, a.visibility::text AS visibility, a.created_at,
                   a.derivative_status, a.derivative_error_code,
                   COALESCE(jsonb_agg(jsonb_build_object(
                     'purpose', f.purpose::text, 'mediaType', f.media_type, 'sizeBytes', f.size_bytes
                   )) FILTER (WHERE f.id IS NOT NULL), '[]'::jsonb) AS files
            FROM assets a LEFT JOIN asset_files f ON f.asset_id = a.id AND f.purpose <> 'render_manifest'
            WHERE a.id = :asset_id AND a.owner_id = :owner_id
            GROUP BY a.id
            """
        ),
        {"asset_id": asset_id, "owner_id": owner_id},
    )
    row = result.mappings().one_or_none()
    return _asset_record(row) if row is not None else None


async def list_owned(
    connection: AsyncConnection,
    *,
    owner_id: UUID,
    limit: int,
    status: str | None = None,
    visibility: str | None = None,
    media_type: str | None = None,
    before_created_at: datetime | None = None,
    before_id: UUID | None = None,
) -> list[AssetRecord]:
    predicate = "a.owner_id = :owner_id"
    params: dict[str, object] = {"owner_id": owner_id, "limit": limit}
    if status is None:
        predicate += " AND a.status <> 'deleted'"
    else:
        predicate += " AND a.status::text = :status"
        params["status"] = status
    if media_type is not None:
        predicate += " AND a.media_type::text = :media_type"
        params["media_type"] = media_type
    if visibility is None:
        predicate += " AND a.visibility = 'private'"
    else:
        predicate += " AND a.visibility::text = :visibility"
        params["visibility"] = visibility
    if before_created_at is not None and before_id is not None:
        predicate += " AND (a.created_at, a.id) < (:before_created_at, :before_id)"
        params.update({"before_created_at": before_created_at, "before_id": before_id})
    result = await connection.execute(
        text(
            f"""
            SELECT a.id, a.owner_id, a.recipe_id, a.media_type::text AS media_type,
                   a.status::text AS status, a.visibility::text AS visibility, a.created_at,
                   a.derivative_status, a.derivative_error_code,
                   COALESCE(jsonb_agg(jsonb_build_object(
                     'purpose', f.purpose::text, 'mediaType', f.media_type, 'sizeBytes', f.size_bytes
                   )) FILTER (WHERE f.id IS NOT NULL), '[]'::jsonb) AS files
            FROM assets a LEFT JOIN asset_files f ON f.asset_id = a.id AND f.purpose <> 'render_manifest'
            WHERE {predicate}
            GROUP BY a.id ORDER BY a.created_at DESC, a.id DESC LIMIT :limit
            """
        ),
        params,
    )
    return [_asset_record(row) for row in result.mappings().all()]


async def find_media_source(connection: AsyncConnection, *, asset_id: UUID) -> MediaSource | None:
    asset_row = await connection.execute(
        text(
            """
            SELECT a.id, a.media_type::text AS media_type, a.status::text AS status,
              m.object_key AS master_object_key, m.media_type AS master_media_type
            FROM assets a
            JOIN asset_files m ON m.asset_id = a.id AND m.purpose = 'master'
            WHERE a.id = :asset_id
            """
        ),
        {"asset_id": asset_id},
    )
    row = asset_row.mappings().one_or_none()
    if row is None:
        return None
    purpose_rows = await connection.execute(
        text("SELECT purpose::text FROM asset_files WHERE asset_id = :asset_id"), {"asset_id": asset_id}
    )
    return MediaSource(
        asset_id=row["id"],
        media_type=str(row["media_type"]),
        status=str(row["status"]),
        master_object_key=str(row["master_object_key"]),
        master_media_type=str(row["master_media_type"]),
        existing_purposes=frozenset(str(value) for value in purpose_rows.scalars()),
    )


async def add_derivative(
    connection: AsyncConnection,
    *,
    asset_id: UUID,
    purpose: str,
    file_id: UUID,
    object_key: str,
    sha256: str,
    size_bytes: int,
    media_type: str,
) -> bool:
    locked = await connection.execute(
        text("SELECT id FROM assets WHERE id = :asset_id AND status = 'ready' FOR UPDATE"),
        {"asset_id": asset_id},
    )
    if locked.scalar_one_or_none() is None:
        return False
    await connection.execute(
        text(
            """
            INSERT INTO asset_files (id, asset_id, purpose, object_key, sha256, size_bytes, media_type)
            VALUES (:id, :asset_id, CAST(:purpose AS asset_file_purpose), :object_key,
                    :sha256, :size_bytes, :media_type)
            ON CONFLICT (asset_id, purpose) DO NOTHING
            """
        ),
        {
            "id": file_id,
            "asset_id": asset_id,
            "purpose": purpose,
            "object_key": object_key,
            "sha256": sha256,
            "size_bytes": size_bytes,
            "media_type": media_type,
        },
    )
    return True


async def mark_derivatives_ready_if_complete(
    connection: AsyncConnection, *, asset_id: UUID, expected_purposes: frozenset[str]
) -> bool:
    """Ready only after every media-type-required derivative is durably recorded."""
    result = await connection.execute(
        text(
            """
            UPDATE assets
            SET derivative_status = 'ready', derivative_error_code = NULL, derivative_updated_at = now()
            WHERE id = :asset_id AND status = 'ready'
              AND NOT EXISTS (
                SELECT 1 FROM unnest(CAST(:expected_purposes AS text[])) AS required(purpose)
                WHERE NOT EXISTS (
                  SELECT 1 FROM asset_files f
                  WHERE f.asset_id = assets.id AND f.purpose::text = required.purpose
                )
              )
            """
        ),
        {"asset_id": asset_id, "expected_purposes": list(expected_purposes)},
    )
    return result.rowcount == 1


async def mark_derivatives_failed(
    connection: AsyncConnection, *, asset_id: UUID, error_code: str
) -> bool:
    result = await connection.execute(
        text(
            """
            UPDATE assets
            SET derivative_status = 'failed', derivative_error_code = :error_code, derivative_updated_at = now()
            WHERE id = :asset_id AND status = 'ready'
            """
        ),
        {"asset_id": asset_id, "error_code": error_code[:120]},
    )
    return result.rowcount == 1


async def find_owned_read_record(
    connection: AsyncConnection, *, asset_id: UUID, owner_id: UUID
) -> AssetReadRecord | None:
    return await _find_read_record(connection, asset_id=asset_id, owner_id=owner_id)


async def find_ready_preview_record(
    connection: AsyncConnection, *, asset_id: UUID
) -> AssetReadRecord | None:
    return await _find_read_record(
        connection,
        asset_id=asset_id,
        owner_id=None,
        required_status="ready",
        required_visibility="private",
    )


async def find_publishable_read_record(
    connection: AsyncConnection, *, asset_id: UUID, owner_id: UUID
) -> AssetReadRecord | None:
    return await _find_read_record(
        connection,
        asset_id=asset_id,
        owner_id=owner_id,
        required_status="ready",
        required_visibility="private",
    )


async def _find_read_record(
    connection: AsyncConnection,
    *,
    asset_id: UUID,
    owner_id: UUID | None,
    required_status: str | None = None,
    required_visibility: str | None = None,
) -> AssetReadRecord | None:
    predicates = ["a.id = :asset_id"]
    params: dict[str, object] = {"asset_id": asset_id}
    if owner_id is not None:
        predicates.append("a.owner_id = :owner_id")
        params["owner_id"] = owner_id
    if required_status is not None:
        predicates.append("a.status::text = :required_status")
        params["required_status"] = required_status
    if required_visibility is not None:
        predicates.append("a.visibility::text = :required_visibility")
        params["required_visibility"] = required_visibility
    result = await connection.execute(
        text(
            f"""
            SELECT a.id, a.owner_id, a.media_type::text AS media_type,
                   a.status::text AS status, a.visibility::text AS visibility,
                   COALESCE(jsonb_object_agg(f.purpose::text, f.object_key)
                     FILTER (WHERE f.purpose IN ('thumbnail', 'watermarked_preview', 'video_poster')),
                     '{{}}'::jsonb) AS derivative_keys
            FROM assets a
            LEFT JOIN asset_files f ON f.asset_id = a.id
            WHERE {' AND '.join(predicates)}
            GROUP BY a.id
            """
        ),
        params,
    )
    row = result.mappings().one_or_none()
    if row is None:
        return None
    data = dict(row)
    return AssetReadRecord(
        id=data["id"],
        owner_id=data["owner_id"],
        media_type=str(data["media_type"]),
        status=str(data["status"]),
        visibility=str(data["visibility"]),
        derivative_keys={str(key): str(value) for key, value in dict(data["derivative_keys"]).items()},
    )


async def referenced_object_keys(connection: AsyncConnection, *, object_keys: list[str]) -> set[str]:
    if not object_keys:
        return set()
    rows = await connection.execute(
        text("SELECT object_key FROM asset_files WHERE object_key = ANY(CAST(:keys AS text[]))"),
        {"keys": object_keys},
    )
    return {str(key) for key in rows.scalars()}


async def find_download_asset(connection: AsyncConnection, *, asset_id: UUID) -> DownloadAsset | None:
    result = await connection.execute(
        text(
            """
            SELECT a.id, a.owner_id, a.status::text AS status, m.object_key AS master_object_key
            FROM assets a LEFT JOIN asset_files m ON m.asset_id = a.id AND m.purpose = 'master'
            WHERE a.id = :asset_id
            """
        ),
        {"asset_id": asset_id},
    )
    row = result.mappings().one_or_none()
    if row is None:
        return None
    return DownloadAsset(
        asset_id=row["id"], owner_id=row["owner_id"], status=str(row["status"]), master_object_key=row["master_object_key"]
    )


async def change_visibility(
    connection: AsyncConnection, *, asset_id: UUID, owner_id: UUID, visibility: str
) -> MutationResult:
    asset = await find_owned_locked(connection, asset_id=asset_id, owner_id=owner_id)
    if asset is None:
        return MutationResult(asset=None, code="not_found", cleanup_keys=[])
    if await _has_non_archived_listing(connection, asset_id=asset_id):
        return MutationResult(asset=None, code="listed", cleanup_keys=[])
    if asset.status == "deleted":
        return MutationResult(asset=None, code="deleted", cleanup_keys=[])
    await connection.execute(
        text("UPDATE assets SET visibility = CAST(:visibility AS asset_visibility) WHERE id = :asset_id"),
        {"visibility": visibility, "asset_id": asset_id},
    )
    return MutationResult(asset=replace(asset, visibility=visibility), code="ok", cleanup_keys=[])


async def soft_delete(connection: AsyncConnection, *, asset_id: UUID, owner_id: UUID) -> MutationResult:
    asset = await find_owned_locked(connection, asset_id=asset_id, owner_id=owner_id)
    if asset is None:
        return MutationResult(asset=None, code="not_found", cleanup_keys=[])
    if asset.status == "deleted":
        return MutationResult(asset=asset, code="ok", cleanup_keys=[])
    if asset.status not in {"ready", "failed"}:
        return MutationResult(asset=None, code="invalid_state", cleanup_keys=[])
    if await _has_non_archived_listing(connection, asset_id=asset_id):
        return MutationResult(asset=None, code="listed", cleanup_keys=[])
    await connection.execute(
        text("UPDATE assets SET status = 'deleted', visibility = 'hidden' WHERE id = :asset_id"),
        {"asset_id": asset_id},
    )
    retained = await _requires_master_retention(connection, asset_id=asset_id)
    cleanup_keys: list[str] = []
    if not retained:
        deleted_files = await connection.execute(
            text("DELETE FROM asset_files WHERE asset_id = :asset_id RETURNING object_key"),
            {"asset_id": asset_id},
        )
        cleanup_keys = [str(key) for key in deleted_files.scalars()]
    return MutationResult(asset=asset, code="ok", cleanup_keys=cleanup_keys)


async def find_owned_locked(
    connection: AsyncConnection, *, asset_id: UUID, owner_id: UUID
) -> AssetRecord | None:
    await connection.execute(
        text("SELECT id FROM assets WHERE id = :asset_id AND owner_id = :owner_id FOR UPDATE"),
        {"asset_id": asset_id, "owner_id": owner_id},
    )
    return await find_owned(connection, asset_id=asset_id, owner_id=owner_id)


async def _has_non_archived_listing(connection: AsyncConnection, *, asset_id: UUID) -> bool:
    return bool(
        await connection.scalar(
            text("SELECT EXISTS (SELECT 1 FROM listings WHERE asset_id = :asset_id AND status <> 'archived')"),
            {"asset_id": asset_id},
        )
    )


async def _requires_master_retention(connection: AsyncConnection, *, asset_id: UUID) -> bool:
    return bool(
        await connection.scalar(
            text(
                """
                SELECT EXISTS (SELECT 1 FROM order_items WHERE asset_id = :asset_id)
                    OR EXISTS (SELECT 1 FROM entitlements WHERE asset_id = :asset_id AND status = 'active')
                """
            ),
            {"asset_id": asset_id},
        )
    )


def _asset_record(row: object) -> AssetRecord:
    data = dict(row)  # type: ignore[arg-type]
    return AssetRecord(
        id=data["id"],
        owner_id=data["owner_id"],
        recipe_id=data["recipe_id"],
        media_type=str(data["media_type"]),
        status=str(data["status"]),
        visibility=str(data["visibility"]),
        created_at=data["created_at"],
        files=list(data["files"]),
        derivative_status=str(data.get("derivative_status", "pending")),
        derivative_error_code=(
            str(data["derivative_error_code"])
            if data.get("derivative_error_code") is not None
            else None
        ),
    )
