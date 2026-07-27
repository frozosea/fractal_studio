"""Render-job state machine and quota-reservation PostgreSQL persistence."""

from __future__ import annotations

import json
from dataclasses import dataclass
from datetime import UTC, datetime, timedelta
from uuid import UUID, uuid4

from sqlalchemy import text
from sqlalchemy.ext.asyncio import AsyncConnection


TERMINAL_STATUSES = frozenset({"completed", "failed", "cancelled"})


@dataclass(frozen=True, slots=True)
class RenderJobRecord:
    id: UUID
    owner_id: UUID
    recipe_id: UUID
    status: str
    compute_run_id: str | None
    output_kind: str
    output_spec: dict[str, object]
    mapping_version: str
    compute_request: dict[str, object]
    progress_percent: int
    compute_result: dict[str, object] | None
    selected_artifact_ids: list[str] | None
    error_code: str | None
    created_at: datetime
    canonical_spec: dict[str, object] | None = None
    asset_id: UUID | None = None


def _record(row: object) -> RenderJobRecord:
    value = dict(row)  # type: ignore[arg-type]
    return RenderJobRecord(
        id=value["id"],
        owner_id=value["owner_id"],
        recipe_id=value["recipe_id"],
        status=str(value["status"]),
        compute_run_id=value["compute_run_id"],
        output_kind=str(value["output_kind"]),
        output_spec=value["output_spec_json"],
        mapping_version=str(value["mapping_version"]),
        compute_request=value["compute_request_json"],
        progress_percent=int(value["progress_percent"]),
        compute_result=value["compute_result_json"],
        selected_artifact_ids=value["selected_artifact_ids_json"],
        error_code=value["error_code"],
        created_at=value["created_at"],
        canonical_spec=value.get("canonical_spec"),
        asset_id=value.get("asset_id"),
    )


_RETURNING = """
id, owner_id, recipe_id, status::text AS status, compute_run_id, output_kind::text AS output_kind,
output_spec_json, mapping_version, compute_request_json, progress_percent, compute_result_json,
selected_artifact_ids_json, error_code, created_at
"""

_JOB_COLUMNS = """
j.id, j.owner_id, j.recipe_id, j.status::text AS status, j.compute_run_id,
j.output_kind::text AS output_kind, j.output_spec_json, j.mapping_version,
j.compute_request_json, j.progress_percent, j.compute_result_json,
j.selected_artifact_ids_json, j.error_code, j.created_at
"""


async def find_recipe_owned(connection: AsyncConnection, *, recipe_id: UUID, owner_id: UUID) -> dict[str, object] | None:
    result = await connection.execute(
        text(
            """
            SELECT id, canonical_spec FROM fractal_recipes
            WHERE id = :recipe_id AND owner_id = :owner_id
            """
        ),
        {"recipe_id": recipe_id, "owner_id": owner_id},
    )
    row = result.mappings().one_or_none()
    return dict(row) if row is not None else None


async def create(
    connection: AsyncConnection,
    *,
    job_id: UUID,
    owner_id: UUID,
    recipe_id: UUID,
    output_kind: str,
    output_spec: dict[str, object],
    mapping_version: str,
    compute_request: dict[str, object],
    idempotency_key: str,
) -> RenderJobRecord:
    result = await connection.execute(
        text(
            f"""
            INSERT INTO render_jobs (
              id, owner_id, recipe_id, status, idempotency_key, output_kind, output_spec_json,
              mapping_version, compute_request_json, progress_percent
            ) VALUES (
              :id, :owner_id, :recipe_id, 'queued', :idempotency_key,
              CAST(:output_kind AS render_output_kind), CAST(:output_spec AS jsonb),
              :mapping_version, CAST(:compute_request AS jsonb), 0
            ) RETURNING {_RETURNING}
            """
        ),
        {
            "id": job_id,
            "owner_id": owner_id,
            "recipe_id": recipe_id,
            "idempotency_key": idempotency_key,
            "output_kind": output_kind,
            "output_spec": json.dumps(output_spec, separators=(",", ":"), sort_keys=True),
            "mapping_version": mapping_version,
            "compute_request": json.dumps(compute_request, separators=(",", ":"), sort_keys=True),
        },
    )
    return _record(result.mappings().one())


async def create_reservation(
    connection: AsyncConnection, *, owner_id: UUID, job_id: UUID, units: int = 1
) -> None:
    await connection.execute(
        text(
            """
            INSERT INTO quota_reservations (id, user_id, render_job_id, quota_kind, units, status, expires_at)
            VALUES (:id, :owner_id, :job_id, 'render', :units, 'reserved', :expires_at)
            """
        ),
        {
            "id": uuid4(),
            "owner_id": owner_id,
            "job_id": job_id,
            "units": units,
            "expires_at": datetime.now(UTC) + timedelta(hours=24),
        },
    )


async def lock_user_reserved_units(connection: AsyncConnection, *, owner_id: UUID) -> int:
    """Lock owner first: serialises active-render quota reservations without global isolation."""
    await connection.execute(text("SELECT id FROM users WHERE id = :owner_id FOR UPDATE"), {"owner_id": owner_id})
    result = await connection.scalar(
        text("SELECT COALESCE(sum(units), 0) FROM quota_reservations WHERE user_id = :owner_id AND status = 'reserved'"),
        {"owner_id": owner_id},
    )
    return int(result or 0)


async def find_owned(connection: AsyncConnection, *, job_id: UUID, owner_id: UUID) -> RenderJobRecord | None:
    result = await connection.execute(
        text(
            f"""
            SELECT {_JOB_COLUMNS}, a.id AS asset_id
            FROM render_jobs j LEFT JOIN assets a ON a.render_job_id = j.id
            WHERE j.id = :job_id AND j.owner_id = :owner_id
            """
        ),
        {"job_id": job_id, "owner_id": owner_id},
    )
    row = result.mappings().one_or_none()
    return _record(row) if row is not None else None


async def lock_for_worker(connection: AsyncConnection, *, job_id: UUID) -> RenderJobRecord | None:
    result = await connection.execute(
        text(
            f"""
            SELECT {_JOB_COLUMNS}, r.canonical_spec
            FROM render_jobs j JOIN fractal_recipes r ON r.id = j.recipe_id
            WHERE j.id = :job_id FOR UPDATE
            """
        ),
        {"job_id": job_id},
    )
    row = result.mappings().one_or_none()
    return _record(row) if row is not None else None


async def set_status(
    connection: AsyncConnection,
    *,
    job_id: UUID,
    expected: set[str],
    next_status: str,
    error_code: str | None = None,
) -> bool:
    result = await connection.execute(
        text(
            """
            UPDATE render_jobs
            SET status = CAST(:next_status AS render_job_status), error_code = COALESCE(:error_code, error_code)
            WHERE id = :job_id AND status::text = ANY(:expected)
            """
        ),
        {"job_id": job_id, "expected": list(expected), "next_status": next_status, "error_code": error_code},
    )
    return result.rowcount == 1


async def save_submission(
    connection: AsyncConnection, *, job_id: UUID, run_id: str
) -> RenderJobRecord | None:
    result = await connection.execute(
        text(
            f"""
            UPDATE render_jobs
            SET compute_run_id = :run_id, status = 'running'
            WHERE id = :job_id AND status = 'submitting'
            RETURNING {_RETURNING}
            """
        ),
        {"job_id": job_id, "run_id": run_id},
    )
    row = result.mappings().one_or_none()
    return _record(row) if row is not None else None


async def save_run_id_if_cancelling(connection: AsyncConnection, *, job_id: UUID, run_id: str) -> bool:
    result = await connection.execute(
        text(
            """
            UPDATE render_jobs SET compute_run_id = :run_id
            WHERE id = :job_id AND status = 'cancel_requested' AND compute_run_id IS NULL
            """
        ),
        {"job_id": job_id, "run_id": run_id},
    )
    return result.rowcount == 1


async def update_progress(connection: AsyncConnection, *, job_id: UUID, progress_percent: int) -> None:
    await connection.execute(
        text(
            """
            UPDATE render_jobs SET progress_percent = GREATEST(progress_percent, :progress_percent)
            WHERE id = :job_id AND status = 'running'
            """
        ),
        {"job_id": job_id, "progress_percent": progress_percent},
    )


async def save_compute_success(
    connection: AsyncConnection,
    *,
    job_id: UUID,
    result_metadata: dict[str, object],
    selected_artifact_ids: list[str],
) -> bool:
    result = await connection.execute(
        text(
            """
            UPDATE render_jobs
            SET status = 'compute_succeeded', progress_percent = 100,
                compute_result_json = CAST(:result AS jsonb),
                selected_artifact_ids_json = CAST(:selected AS jsonb)
            WHERE id = :job_id AND status = 'running'
            """
        ),
        {
            "job_id": job_id,
            "result": json.dumps(result_metadata, separators=(",", ":"), sort_keys=True),
            "selected": json.dumps(selected_artifact_ids),
        },
    )
    return result.rowcount == 1


async def release_reservation(connection: AsyncConnection, *, job_id: UUID) -> bool:
    result = await connection.execute(
        text(
            """
            UPDATE quota_reservations SET status = 'released'
            WHERE render_job_id = :job_id AND status = 'reserved'
            """
        ),
        {"job_id": job_id},
    )
    return result.rowcount == 1


async def cancel_and_release(connection: AsyncConnection, *, job_id: UUID) -> bool:
    changed = await set_status(
        connection, job_id=job_id, expected={"queued", "submitting", "running", "cancel_requested"}, next_status="cancelled"
    )
    if changed:
        await release_reservation(connection, job_id=job_id)
    return changed


async def fail_and_release(connection: AsyncConnection, *, job_id: UUID, error_code: str) -> bool:
    changed = await set_status(
        connection,
        job_id=job_id,
        expected={"queued", "submitting", "running", "compute_succeeded", "ingesting"},
        next_status="failed",
        error_code=error_code,
    )
    if changed:
        await release_reservation(connection, job_id=job_id)
        return True
    # A user cancellation wins over an infrastructure failure observed concurrently.
    return await cancel_and_release(connection, job_id=job_id)


async def find_expired_reservation_job_ids(connection: AsyncConnection) -> list[UUID]:
    result = await connection.execute(
        text(
            """
            SELECT render_job_id FROM quota_reservations
            WHERE status = 'reserved' AND expires_at <= now()
            FOR UPDATE SKIP LOCKED
            """
        )
    )
    return list(result.scalars())


async def expire_reservation_and_terminalize(connection: AsyncConnection, *, job_id: UUID) -> bool:
    result = await connection.execute(
        text(
            """
            SELECT q.status::text AS reservation_status, q.expires_at, j.status::text AS job_status
            FROM quota_reservations q JOIN render_jobs j ON j.id = q.render_job_id
            WHERE q.render_job_id = :job_id
            FOR UPDATE OF q, j
            """
        ),
        {"job_id": job_id},
    )
    row = result.mappings().one_or_none()
    if row is None or row["reservation_status"] != "reserved":
        return False
    if row["expires_at"] > datetime.now(UTC):
        return False
    job_status = str(row["job_status"])
    if job_status in TERMINAL_STATUSES:
        await release_reservation(connection, job_id=job_id)
        return True
    next_status = "cancelled" if job_status == "cancel_requested" else "failed"
    await connection.execute(
        text(
            """
            UPDATE render_jobs SET status = CAST(:next_status AS render_job_status),
              error_code = CASE WHEN :next_status = 'failed' THEN 'quota_reservation_expired' ELSE error_code END
            WHERE id = :job_id
            """
        ),
        {"job_id": job_id, "next_status": next_status},
    )
    await connection.execute(
        text(
            "UPDATE quota_reservations SET status = 'expired' WHERE render_job_id = :job_id AND status = 'reserved'"
        ),
        {"job_id": job_id},
    )
    return True
