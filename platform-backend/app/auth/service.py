"""Registration, password login and creator-profile application service."""

from __future__ import annotations

import base64
import binascii
import hashlib
import hmac
import os
import uuid
from typing import Any

from fastapi import HTTPException, Request, status
from sqlalchemy import text
from sqlalchemy.exc import IntegrityError
from sqlalchemy.ext.asyncio import AsyncConnection

from app.auth import (
    creator_profile_repository,
    session_service,
    user_repository,
    user_role_repository,
)
from app.auth.models import AccessPrincipal, CreatorProfileInput, UserView
from app.core import audit_writer, idempotency_service
from app.core.access_middleware import session_token_from
from app.core.db import get_engine
from app.core.request_context import request_id


_SCRYPT_N = 2**17
_SCRYPT_R = 8
_SCRYPT_P = 1
_SCRYPT_MAX_MEMORY = 256 * 1024 * 1024


def _hash_password(password: str, *, salt: bytes | None = None) -> str:
    """Use stdlib scrypt; hash string contains parameters but never leaves persistence."""
    salt = salt or os.urandom(16)
    derived = hashlib.scrypt(
        password.encode(),
        salt=salt,
        n=_SCRYPT_N,
        r=_SCRYPT_R,
        p=_SCRYPT_P,
        maxmem=_SCRYPT_MAX_MEMORY,
    )
    return "scrypt${}${}${}${}${}".format(
        _SCRYPT_N,
        _SCRYPT_R,
        _SCRYPT_P,
        base64.urlsafe_b64encode(salt).decode(),
        base64.urlsafe_b64encode(derived).decode(),
    )


def _verify_password(password: str, encoded: str) -> bool:
    try:
        algorithm, n, r, p, salt_b64, hash_b64 = encoded.split("$")
        if algorithm != "scrypt":
            return False
        actual = hashlib.scrypt(
            password.encode(),
            salt=base64.urlsafe_b64decode(salt_b64),
            n=int(n),
            r=int(r),
            p=int(p),
            maxmem=_SCRYPT_MAX_MEMORY,
        )
        return hmac.compare_digest(actual, base64.urlsafe_b64decode(hash_b64))
    except (binascii.Error, ValueError, TypeError):
        return False


def _password_needs_rehash(encoded: str) -> bool:
    try:
        algorithm, n, r, p, _salt_b64, _hash_b64 = encoded.split("$")
        return (
            algorithm != "scrypt"
            or int(n) != _SCRYPT_N
            or int(r) != _SCRYPT_R
            or int(p) != _SCRYPT_P
        )
    except (ValueError, TypeError):
        return True


async def _user_view(connection: AsyncConnection, user_id: uuid.UUID) -> UserView:
    user = await user_repository.find_active_by_id(connection, user_id)
    if user is None:
        raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="unauthenticated")
    profile = await creator_profile_repository.find(connection, user_id)
    member = await connection.scalar(
        text("SELECT status FROM memberships WHERE user_id = :uid"), {"uid": user_id}
    )
    return UserView(
        id=user["id"],
        email=str(user["email"]),
        status=str(user["status"]),
        roles=await user_role_repository.list_roles(connection, user_id),
        member=member == "active",
        creator_profile=(
            {"handle": str(profile["handle"]), "display_name": str(profile["display_name"])}
            if profile
            else None
        ),
    )


async def register(email: str, password: str, request: Request) -> tuple[UserView, str]:
    user_id = uuid.uuid4()
    try:
        async with get_engine().begin() as connection:
            await user_repository.create(
                connection, user_id=user_id, email=email, password_hash=_hash_password(password)
            )
            session_token = await session_service.create(
                connection, user_id=user_id, request=request
            )
            await audit_writer.record_user_action(
                connection,
                actor_user_id=user_id,
                action="auth.registered",
                subject_type="user",
                subject_id=user_id,
                request_id_value=request_id(request),
            )
            view = await _user_view(connection, user_id)
    except IntegrityError as error:
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT, detail="email_already_registered"
        ) from error
    return view, session_token


async def login(email: str, password: str, request: Request) -> tuple[UserView, str]:
    async with get_engine().begin() as connection:
        user = await user_repository.find_by_email(connection, email)
        encoded_password = str(user["password_hash"]) if user is not None else ""
        if user is None or not _verify_password(password, encoded_password):
            raise HTTPException(
                status_code=status.HTTP_401_UNAUTHORIZED, detail="invalid_credentials"
            )
        # Only reveal the disabled state after the caller proves knowledge of
        # the password. Otherwise any unauthenticated caller could enumerate
        # disabled accounts by submitting an arbitrary password.
        if user["status"] != "active":
            raise HTTPException(
                status_code=status.HTTP_401_UNAUTHORIZED, detail="account_disabled"
            )
        user_id = user["id"]
        if _password_needs_rehash(encoded_password):
            await user_repository.update_password_hash(
                connection,
                user_id=user_id,
                password_hash=_hash_password(password),
            )
        raw_token, source = session_token_from(request)
        existing = await session_service.resolve(connection, raw_token)
        if existing is not None and existing.user_id == user_id:
            session_token = await session_service.rotate(connection, existing, request)
        else:
            # A session for a *different* user is only this tab's to end when the
            # tab presented it itself as a bearer token. The cookie is shared by
            # every tab in the window, so revoking what it points at would sign
            # out whichever sibling tab is using that account.
            if existing is not None and source == "bearer":
                await session_service.revoke(connection, existing.session_id)
            session_token = await session_service.create(
                connection, user_id=user_id, request=request
            )
        await audit_writer.record_user_action(
            connection,
            actor_user_id=user_id,
            action="auth.logged_in",
            subject_type="user",
            subject_id=user_id,
            request_id_value=request_id(request),
        )
        view = await _user_view(connection, user_id)
    return view, session_token


async def logout(principal: AccessPrincipal, request: Request) -> None:
    async with get_engine().begin() as connection:
        await session_service.revoke(connection, principal.session_id)
        await audit_writer.record_user_action(
            connection,
            actor_user_id=principal.user_id,
            action="auth.logged_out",
            subject_type="session",
            subject_id=principal.session_id,
            request_id_value=request_id(request),
        )


async def current_user(principal: AccessPrincipal) -> UserView:
    async with get_engine().connect() as connection:
        return await _user_view(connection, principal.user_id)


async def upsert_creator_profile(
    principal: AccessPrincipal,
    payload: CreatorProfileInput,
    idempotency_key: str,
    request: Request,
) -> tuple[dict[str, Any], str | None, bool, dict[str, str]]:
    """Profile, creator role, audit, session rotation and idempotency commit together."""
    if "admin" in principal.roles:
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT,
            detail="admin_creator_role_conflict",
        )
    try:
        async with get_engine().begin() as connection:
            claim = await idempotency_service.claim(
                connection,
                user_id=principal.user_id,
                scope="auth.creator_profile",
                key=idempotency_key,
                body=payload.model_dump(mode="json", by_alias=True),
            )
            if claim.is_replay:
                return claim.replay_body or {}, None, True, claim.replay_headers or {}
            current_profile = await connection.execute(
                text(
                    "SELECT display_name, display_name_changed_at "
                    "FROM creator_profiles WHERE user_id = :user_id FOR UPDATE"
                ),
                {"user_id": principal.user_id},
            )
            current = current_profile.mappings().one_or_none()
            if (
                current
                and current["display_name"] != payload.display_name
                and current["display_name_changed_at"] is not None
                and current["display_name_changed_at"] > await connection.scalar(
                    text("SELECT CURRENT_TIMESTAMP - INTERVAL '30 days'")
                )
            ):
                raise HTTPException(
                    status_code=status.HTTP_409_CONFLICT,
                    detail="creator_name_change_too_soon",
                )
            await creator_profile_repository.upsert(
                connection,
                user_id=principal.user_id,
                handle=payload.handle,
                display_name=payload.display_name,
            )
            await user_role_repository.grant_creator(connection, principal.user_id)
            await audit_writer.record_user_action(
                connection,
                actor_user_id=principal.user_id,
                action="creator_profile.upserted",
                subject_type="creator_profile",
                subject_id=principal.user_id,
                request_id_value=request_id(request),
            )
            new_session_token = await session_service.rotate(connection, principal, request)
            body = {
                "data": (await _user_view(connection, principal.user_id)).model_dump(
                    mode="json", by_alias=True
                )
            }
            response_headers = {"Cache-Control": "no-store"}
            await idempotency_service.complete(
                connection,
                claim,
                response_status=200,
                response_body=body,
                response_headers=response_headers,
            )
    except IntegrityError as error:
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT, detail="handle_already_registered"
        ) from error
    return body, new_session_token, False, response_headers
