"""Persistence for idempotency records."""

from __future__ import annotations

import json
import uuid
from datetime import datetime
from typing import Any

from sqlalchemy import text
from sqlalchemy.ext.asyncio import AsyncConnection


async def insert_claim(
    connection: AsyncConnection,
    *,
    record_id: uuid.UUID,
    user_id: uuid.UUID,
    scope: str,
    key: str,
    request_hash: str,
    lease_owner: str,
    expires_at: datetime,
    lease_until: datetime,
) -> bool:
    result = await connection.execute(
        text(
            "INSERT INTO idempotency_records (id, user_id, scope, idempotency_key, request_hash, "
            "lease_owner, lease_until, expires_at) VALUES "
            "(:id, :user_id, :scope, :key, :request_hash, :lease_owner, :lease_until, :expires_at) "
            "ON CONFLICT (user_id, scope, idempotency_key) DO NOTHING RETURNING id"
        ),
        {
            "id": record_id,
            "user_id": user_id,
            "scope": scope,
            "key": key,
            "request_hash": request_hash,
            "lease_owner": lease_owner,
            "lease_until": lease_until,
            "expires_at": expires_at,
        },
    )
    return result.scalar_one_or_none() is not None


async def lock(connection: AsyncConnection, *, user_id: uuid.UUID, scope: str, key: str) -> dict[str, Any]:
    result = await connection.execute(
        text(
            "SELECT id, request_hash, status::text, response_json, response_status, response_headers_json, "
            "lease_until, expires_at FROM idempotency_records WHERE user_id = :user_id AND scope = :scope "
            "AND idempotency_key = :key FOR UPDATE"
        ),
        {"user_id": user_id, "scope": scope, "key": key},
    )
    return dict(result.mappings().one())


async def reclaim(
    connection: AsyncConnection,
    *,
    record_id: uuid.UUID,
    request_hash: str,
    lease_owner: str,
    lease_until: datetime,
    expires_at: datetime,
) -> bool:
    """Reuse a locked stale row; uniqueness remains stable without permanent key poisoning."""
    result = await connection.execute(
        text(
            "UPDATE idempotency_records SET request_hash = :request_hash, status = 'processing', "
            "response_json = NULL, response_status = NULL, response_headers_json = NULL, completed_at = NULL, "
            "lease_owner = :lease_owner, lease_until = :lease_until, expires_at = :expires_at WHERE id = :id"
        ),
        {
            "id": record_id,
            "request_hash": request_hash,
            "lease_owner": lease_owner,
            "lease_until": lease_until,
            "expires_at": expires_at,
        },
    )
    return result.rowcount == 1


async def delete_expired(connection: AsyncConnection, *, batch_size: int = 1_000) -> int:
    """Bound cleanup work so completed idempotency records cannot grow forever."""
    result = await connection.execute(
        text(
            "WITH expired AS ("
            " SELECT id FROM idempotency_records WHERE expires_at <= now() "
            " ORDER BY expires_at FOR UPDATE SKIP LOCKED LIMIT :batch_size"
            ") DELETE FROM idempotency_records WHERE id IN (SELECT id FROM expired)"
        ),
        {"batch_size": batch_size},
    )
    return result.rowcount or 0


async def complete(
    connection: AsyncConnection,
    *,
    record_id: uuid.UUID,
    response_status: int,
    response_json: dict[str, Any] | None,
    response_headers: dict[str, str],
) -> None:
    await connection.execute(
        text(
            "UPDATE idempotency_records SET status = 'completed', response_status = :response_status, "
            "response_json = CAST(:response_json AS jsonb), response_headers_json = CAST(:response_headers AS jsonb), "
            "completed_at = now(), lease_owner = NULL, lease_until = NULL WHERE id = :id"
        ),
        {
            "id": record_id,
            "response_status": response_status,
            "response_json": json.dumps(response_json) if response_json is not None else None,
            "response_headers": json.dumps(response_headers),
        },
    )
