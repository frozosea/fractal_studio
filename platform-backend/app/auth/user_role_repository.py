"""Multi-role persistence and lookup."""

from __future__ import annotations

from uuid import UUID

from sqlalchemy import text
from sqlalchemy.ext.asyncio import AsyncConnection


async def list_roles(connection: AsyncConnection, user_id: UUID) -> list[str]:
    result = await connection.execute(
        text("SELECT role::text FROM user_roles WHERE user_id = :user_id ORDER BY role"),
        {"user_id": user_id},
    )
    return [row[0] for row in result]


async def grant_creator(connection: AsyncConnection, user_id: UUID) -> None:
    await connection.execute(
        text(
            "INSERT INTO user_roles (user_id, role) VALUES (:user_id, 'creator') "
            "ON CONFLICT (user_id, role) DO NOTHING"
        ),
        {"user_id": user_id},
    )
