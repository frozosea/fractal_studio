"""Append-only audit persistence."""

from __future__ import annotations

import uuid
from typing import Any

from sqlalchemy import text
from sqlalchemy.ext.asyncio import AsyncConnection


async def append(
    connection: AsyncConnection,
    *,
    actor_user_id: uuid.UUID | None,
    actor_type: str,
    action: str,
    subject_type: str,
    subject_id: uuid.UUID,
    metadata: dict[str, Any],
) -> None:
    await connection.execute(
        text(
            "INSERT INTO audit_events (id, actor_user_id, actor_type, action, subject_type, subject_id, metadata_json) "
            "VALUES (:id, :actor_user_id, :actor_type, :action, :subject_type, :subject_id, "
            "CAST(:metadata AS jsonb))"
        ),
        {
            "id": uuid.uuid4(),
            "actor_user_id": actor_user_id,
            "actor_type": actor_type,
            "action": action,
            "subject_type": subject_type,
            "subject_id": subject_id,
            "metadata": __import__("json").dumps(metadata),
        },
    )
