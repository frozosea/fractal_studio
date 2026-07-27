"""Opaque session lifecycle: random browser token, SHA-256 in PostgreSQL only."""

from __future__ import annotations

import hashlib
import secrets
import uuid
from datetime import UTC, datetime, timedelta

from fastapi import Request
from sqlalchemy.ext.asyncio import AsyncConnection

from app.auth import session_repository, user_role_repository
from app.auth.models import AccessPrincipal
from app.core.config import get_settings


def token_hash(raw_token: str) -> str:
    return hashlib.sha256(raw_token.encode()).hexdigest()


def _metadata(request: Request) -> tuple[str, str]:
    ip = request.client.host if request.client else "unknown"
    user_agent = request.headers.get("user-agent", "unknown")
    return token_hash(ip), token_hash(user_agent)


async def create(
    connection: AsyncConnection,
    *,
    user_id: uuid.UUID,
    request: Request,
    rotated_from_session_id: uuid.UUID | None = None,
) -> str:
    raw_token = secrets.token_urlsafe(32)
    ip_hash, ua_hash = _metadata(request)
    await session_repository.create(
        connection,
        session_id=uuid.uuid4(),
        user_id=user_id,
        token_hash=token_hash(raw_token),
        expires_at=datetime.now(UTC) + timedelta(days=get_settings().session_ttl_days),
        created_ip_hash=ip_hash,
        user_agent_hash=ua_hash,
        rotated_from_session_id=rotated_from_session_id,
    )
    return raw_token


async def resolve(connection: AsyncConnection, raw_token: str | None) -> AccessPrincipal | None:
    if not raw_token:
        return None
    session = await session_repository.find_active_by_hash(connection, token_hash(raw_token))
    if session is None:
        return None
    user_id = session["user_id"]
    return AccessPrincipal(
        user_id=user_id,
        session_id=session["session_id"],
        roles=frozenset(await user_role_repository.list_roles(connection, user_id)),
        session_token=raw_token,
    )


async def revoke(connection: AsyncConnection, session_id: uuid.UUID) -> None:
    await session_repository.revoke(connection, session_id)


async def rotate(connection: AsyncConnection, principal: AccessPrincipal, request: Request) -> str:
    await revoke(connection, principal.session_id)
    return await create(
        connection,
        user_id=principal.user_id,
        request=request,
        rotated_from_session_id=principal.session_id,
    )
