"""M3 persistence for asset ingest; no object-store calls happen here."""

from __future__ import annotations

import json
from dataclasses import dataclass
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


async def find_owned(connection: AsyncConnection, *, asset_id: UUID, owner_id: UUID) -> AssetRecord | None:
    result = await connection.execute(
        text(
            """
            SELECT a.id, a.owner_id, a.recipe_id, a.media_type::text AS media_type,
                   a.status::text AS status, a.visibility::text AS visibility, a.created_at,
                   COALESCE(jsonb_agg(jsonb_build_object(
                     'purpose', f.purpose::text, 'mediaType', f.media_type, 'sizeBytes', f.size_bytes
                   )) FILTER (WHERE f.id IS NOT NULL), '[]'::jsonb) AS files
            FROM assets a LEFT JOIN asset_files f ON f.asset_id = a.id
            WHERE a.id = :asset_id AND a.owner_id = :owner_id
            GROUP BY a.id
            """
        ),
        {"asset_id": asset_id, "owner_id": owner_id},
    )
    row = result.mappings().one_or_none()
    return _asset_record(row) if row is not None else None


async def list_owned(
    connection: AsyncConnection, *, owner_id: UUID, limit: int, before_created_at: datetime | None = None, before_id: UUID | None = None
) -> list[AssetRecord]:
    predicate = "a.owner_id = :owner_id"
    params: dict[str, object] = {"owner_id": owner_id, "limit": limit}
    if before_created_at is not None and before_id is not None:
        predicate += " AND (a.created_at, a.id) < (:before_created_at, :before_id)"
        params.update({"before_created_at": before_created_at, "before_id": before_id})
    result = await connection.execute(
        text(
            f"""
            SELECT a.id, a.owner_id, a.recipe_id, a.media_type::text AS media_type,
                   a.status::text AS status, a.visibility::text AS visibility, a.created_at,
                   COALESCE(jsonb_agg(jsonb_build_object(
                     'purpose', f.purpose::text, 'mediaType', f.media_type, 'sizeBytes', f.size_bytes
                   )) FILTER (WHERE f.id IS NOT NULL), '[]'::jsonb) AS files
            FROM assets a LEFT JOIN asset_files f ON f.asset_id = a.id
            WHERE {predicate}
            GROUP BY a.id ORDER BY a.created_at DESC, a.id DESC LIMIT :limit
            """
        ),
        params,
    )
    return [_asset_record(row) for row in result.mappings().all()]


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
    )
