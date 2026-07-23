"""Session persistence; only SHA-256 token hashes cross this boundary."""

from __future__ import annotations

from datetime import datetime
from uuid import UUID

from sqlalchemy import text
from sqlalchemy.ext.asyncio import AsyncConnection


async def create(
    connection: AsyncConnection,
    *,
    session_id: UUID,
    user_id: UUID,
    token_hash: str,
    expires_at: datetime,
    created_ip_hash: str,
    user_agent_hash: str,
    rotated_from_session_id: UUID | None = None,
) -> None:
    await connection.execute(
        text(
            "INSERT INTO sessions ("
            "id, user_id, token_hash, expires_at, created_ip_hash, user_agent_hash, "
            "rotated_from_session_id"
            ") VALUES ("
            ":id, :user_id, :token_hash, :expires_at, :created_ip_hash, :user_agent_hash, "
            ":rotated_from_session_id"
            ")"
        ),
        {
            "id": session_id,
            "user_id": user_id,
            "token_hash": token_hash,
            "expires_at": expires_at,
            "created_ip_hash": created_ip_hash,
            "user_agent_hash": user_agent_hash,
            "rotated_from_session_id": rotated_from_session_id,
        },
    )


async def find_active_by_hash(connection: AsyncConnection, token_hash: str) -> dict[str, object] | None:
    result = await connection.execute(
        text(
            "SELECT s.id AS session_id, s.user_id "
            "FROM sessions s JOIN users u ON u.id = s.user_id "
            "WHERE s.token_hash = :token_hash AND s.revoked_at IS NULL AND s.expires_at > now() "
            "AND u.status = 'active'"
        ),
        {"token_hash": token_hash},
    )
    return result.mappings().one_or_none()


async def revoke(connection: AsyncConnection, session_id: UUID) -> None:
    await connection.execute(
        text("UPDATE sessions SET revoked_at = now() WHERE id = :id AND revoked_at IS NULL"), {"id": session_id}
    )
