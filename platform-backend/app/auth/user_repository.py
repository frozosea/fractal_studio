"""User persistence. Repositories never receive raw session tokens."""

from __future__ import annotations

from uuid import UUID

from sqlalchemy import text
from sqlalchemy.ext.asyncio import AsyncConnection


async def create(connection: AsyncConnection, *, user_id: UUID, email: str, password_hash: str) -> None:
    await connection.execute(
        text("INSERT INTO users (id, email, password_hash) VALUES (:id, :email, :password_hash)"),
        {"id": user_id, "email": email, "password_hash": password_hash},
    )


async def find_by_email(connection: AsyncConnection, email: str) -> dict[str, object] | None:
    result = await connection.execute(
        text("SELECT id, email, password_hash, status FROM users WHERE email = :email"), {"email": email}
    )
    return result.mappings().one_or_none()


async def find_active_by_id(connection: AsyncConnection, user_id: UUID) -> dict[str, object] | None:
    result = await connection.execute(
        text("SELECT id, email, status FROM users WHERE id = :id AND status = 'active'"), {"id": user_id}
    )
    return result.mappings().one_or_none()
