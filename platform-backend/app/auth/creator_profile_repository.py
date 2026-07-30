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


async def find_public_by_handle(connection: AsyncConnection, handle: str) -> dict[str, object] | None:
    """
    Public profile lookup for a creator page, with how many listings they have
    on sale. Matches the handle exactly — the catalogue's `creator` filter is a
    substring match, which would conflate `bob` with `bobby` here.
    """
    result = await connection.execute(
        text(
            """
            SELECT cp.handle, cp.display_name,
                   COUNT(l.id) FILTER (
                       WHERE l.status = 'published' AND l.current_published_version_id IS NOT NULL
                   ) AS published_count
            FROM creator_profiles cp
            LEFT JOIN listings l ON l.creator_id = cp.user_id
            WHERE cp.handle = :handle
            GROUP BY cp.handle, cp.display_name
            """
        ),
        {"handle": handle},
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
