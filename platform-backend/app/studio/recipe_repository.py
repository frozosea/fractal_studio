"""PostgreSQL persistence for immutable fractal recipes."""

from __future__ import annotations

import json
from dataclasses import dataclass
from datetime import datetime
from uuid import UUID, uuid4

from sqlalchemy import text
from sqlalchemy.ext.asyncio import AsyncConnection


@dataclass(frozen=True, slots=True)
class RecipeRecord:
    id: UUID
    owner_id: UUID
    canonical_spec: dict[str, object]
    spec_hash: str
    structure_version: int
    renderer_version: str
    created_at: datetime


def _record(row: object) -> RecipeRecord:
    mapping = dict(row)  # type: ignore[arg-type]
    return RecipeRecord(
        id=mapping["id"],
        owner_id=mapping["owner_id"],
        canonical_spec=mapping["canonical_spec"],
        spec_hash=mapping["spec_hash"],
        structure_version=mapping["structure_version"],
        renderer_version=mapping["renderer_version"],
        created_at=mapping["created_at"],
    )


async def insert_or_find(
    connection: AsyncConnection,
    *,
    owner_id: UUID,
    canonical_spec: dict[str, object],
    spec_hash: str,
    structure_version: int,
    renderer_version: str,
) -> tuple[RecipeRecord, bool]:
    """Insert once per owner/hash. Conflict reads authoritative existing record."""
    inserted = await connection.execute(
        text(
            """
            INSERT INTO fractal_recipes (
              id, owner_id, canonical_spec, spec_hash, structure_version, renderer_version
            ) VALUES (
              :id, :owner_id, CAST(:canonical_spec AS jsonb), :spec_hash,
              :structure_version, :renderer_version
            )
            ON CONFLICT (owner_id, spec_hash) DO NOTHING
            RETURNING id, owner_id, canonical_spec, spec_hash, structure_version, renderer_version, created_at
            """
        ),
        {
            "id": uuid4(),
            "owner_id": owner_id,
            "canonical_spec": json.dumps(canonical_spec, separators=(",", ":"), ensure_ascii=False),
            "spec_hash": spec_hash,
            "structure_version": structure_version,
            "renderer_version": renderer_version,
        },
    )
    row = inserted.mappings().one_or_none()
    if row is not None:
        return _record(row), True
    existing = await connection.execute(
        text(
            """
            SELECT id, owner_id, canonical_spec, spec_hash, structure_version, renderer_version, created_at
            FROM fractal_recipes WHERE owner_id = :owner_id AND spec_hash = :spec_hash
            """
        ),
        {"owner_id": owner_id, "spec_hash": spec_hash},
    )
    return _record(existing.mappings().one()), False


async def list_owned(
    connection: AsyncConnection,
    *,
    owner_id: UUID,
    before_created_at: datetime | None,
    before_id: UUID | None,
    limit: int,
) -> list[RecipeRecord]:
    if before_created_at is None or before_id is None:
        statement = text(
            """
            SELECT id, owner_id, canonical_spec, spec_hash, structure_version, renderer_version, created_at
            FROM fractal_recipes WHERE owner_id = :owner_id
            ORDER BY created_at DESC, id DESC LIMIT :limit
            """
        )
        parameters = {"owner_id": owner_id, "limit": limit}
    else:
        statement = text(
            """
            SELECT id, owner_id, canonical_spec, spec_hash, structure_version, renderer_version, created_at
            FROM fractal_recipes
            WHERE owner_id = :owner_id AND (created_at, id) < (:before_created_at, :before_id)
            ORDER BY created_at DESC, id DESC LIMIT :limit
            """
        )
        parameters = {
            "owner_id": owner_id,
            "before_created_at": before_created_at,
            "before_id": before_id,
            "limit": limit,
        }
    result = await connection.execute(statement, parameters)
    return [_record(row) for row in result.mappings().all()]
