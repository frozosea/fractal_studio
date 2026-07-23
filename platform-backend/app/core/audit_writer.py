"""Transaction-bound audit writer; metadata must contain no credentials or tokens."""

from __future__ import annotations

import uuid
from typing import Any

from sqlalchemy.ext.asyncio import AsyncConnection

from app.core import audit_repository


async def record_user_action(
    connection: AsyncConnection,
    *,
    actor_user_id: uuid.UUID,
    action: str,
    subject_type: str,
    subject_id: uuid.UUID,
    request_id_value: str,
    metadata: dict[str, Any] | None = None,
) -> None:
    safe_metadata = {"requestId": request_id_value, **(metadata or {})}
    await audit_repository.append(
        connection,
        actor_user_id=actor_user_id,
        actor_type="user",
        action=action,
        subject_type=subject_type,
        subject_id=subject_id,
        metadata=safe_metadata,
    )
