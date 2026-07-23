"""Idempotency claim/replay/complete service for browser mutations."""

from __future__ import annotations

import hashlib
import json
import uuid
from dataclasses import dataclass
from datetime import UTC, datetime, timedelta
from typing import Any

from fastapi import HTTPException, status
from sqlalchemy.ext.asyncio import AsyncConnection

from app.core import idempotency_repository
from app.core.config import get_settings


@dataclass(frozen=True, slots=True)
class IdempotencyClaim:
    record_id: uuid.UUID | None
    replay_status: int | None = None
    replay_body: dict[str, Any] | None = None
    replay_headers: dict[str, str] | None = None

    @property
    def is_replay(self) -> bool:
        return self.record_id is None


def request_hash(body: Any) -> str:
    encoded = json.dumps(body, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode()
    return hashlib.sha256(encoded).hexdigest()


async def claim(
    connection: AsyncConnection,
    *,
    user_id: uuid.UUID,
    scope: str,
    key: str,
    body: Any,
) -> IdempotencyClaim:
    if not 1 <= len(key) <= 255:
        raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_ENTITY, detail="invalid_idempotency_key")
    settings = get_settings()
    now = datetime.now(UTC)
    digest = request_hash(body)
    record_id = uuid.uuid4()
    claimed = await idempotency_repository.insert_claim(
        connection,
        record_id=record_id,
        user_id=user_id,
        scope=scope,
        key=key,
        request_hash=digest,
        lease_owner=str(uuid.uuid4()),
        expires_at=now + timedelta(hours=settings.idempotency_ttl_hours),
        lease_until=now + timedelta(seconds=settings.idempotency_lease_seconds),
    )
    if claimed:
        return IdempotencyClaim(record_id=record_id)
    existing = await idempotency_repository.lock(connection, user_id=user_id, scope=scope, key=key)
    if existing["request_hash"] != digest:
        raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail="idempotency_conflict")
    if existing["status"] == "completed":
        return IdempotencyClaim(
            record_id=None,
            replay_status=existing["response_status"],
            replay_body=existing["response_json"],
            replay_headers=existing["response_headers_json"] or {},
        )
    raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail="idempotency_in_progress")


async def complete(
    connection: AsyncConnection,
    claim_result: IdempotencyClaim,
    *,
    response_status: int,
    response_body: dict[str, Any] | None,
    response_headers: dict[str, str] | None = None,
) -> None:
    if claim_result.record_id is None:
        return
    await idempotency_repository.complete(
        connection,
        record_id=claim_result.record_id,
        response_status=response_status,
        response_json=response_body,
        response_headers=response_headers or {},
    )
