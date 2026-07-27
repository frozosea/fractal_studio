"""Creator profile persistence."""

from __future__ import annotations

from uuid import UUID

from sqlalchemy import text
from sqlalchemy.ext.asyncio import AsyncConnection


async def find(connection: AsyncConnection, user_id: UUID) -> dict[str, object] | None:
    result = await connection.execute(
        text("SELECT handle, display_name FROM creator_profiles WHERE user_id = :user_id"),
        {"user_id": user_id},
    )
    return result.mappings().one_or_none()


async def upsert(
    connection: AsyncConnection, *, user_id: UUID, handle: str, display_name: str
) -> None:
    await connection.execute(
        text(
            "INSERT INTO creator_profiles (user_id, handle, display_name) "
            "VALUES (:user_id, :handle, :display_name) "
            "ON CONFLICT (user_id) DO UPDATE "
            "SET handle = EXCLUDED.handle, display_name = EXCLUDED.display_name"
        ),
        {"user_id": user_id, "handle": handle, "display_name": display_name},
    )
