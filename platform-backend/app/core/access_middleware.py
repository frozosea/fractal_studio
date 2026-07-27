"""Opaque-session access dependencies, RBAC and CSRF/Origin guard."""

from __future__ import annotations

import hashlib
import hmac
from collections.abc import Callable

from fastapi import HTTPException, Request, status

from app.auth import session_service
from app.auth.models import AccessPrincipal
from app.core.config import get_settings
from app.core.db import get_engine
from app.core.request_context import user_id_var


async def require_principal(request: Request) -> AccessPrincipal:
    """Resolve fresh session, user state and roles for every protected request."""
    raw_token = request.cookies.get("fs_session")
    async with get_engine().connect() as connection:
        principal = await session_service.resolve(connection, raw_token)
    if principal is None:
        raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="unauthenticated")
    request.state.principal = principal
    request.state.user_id = str(principal.user_id)
    user_id_var.set(str(principal.user_id))
    return principal


def require_role(role: str) -> Callable[..., AccessPrincipal]:
    async def dependency(request: Request) -> AccessPrincipal:
        principal = await require_principal(request)
        if role not in principal.roles:
            raise HTTPException(status_code=status.HTTP_403_FORBIDDEN, detail="forbidden")
        return principal

    return dependency


def csrf_token(raw_session_token: str) -> str:
    secret = get_settings().session_secret.encode()
    return hmac.new(secret, raw_session_token.encode(), hashlib.sha256).hexdigest()


def enforce_same_origin_or_no_origin(request: Request) -> None:
    """Login/register are not approved cross-site flows."""
    origin = request.headers.get("origin")
    if origin and origin != get_settings().api_origin:
        raise HTTPException(status_code=status.HTTP_403_FORBIDDEN, detail="untrusted_origin")


def enforce_origin_and_csrf(request: Request, principal: AccessPrincipal) -> None:
    """Reject unknown Origin and require CSRF proof for trusted cross-origin mutations."""
    origin = request.headers.get("origin")
    if not origin or origin == get_settings().api_origin:
        return
    if origin not in get_settings().trusted_origins:
        raise HTTPException(status_code=status.HTTP_403_FORBIDDEN, detail="untrusted_origin")
    supplied = request.headers.get("x-csrf-token", "")
    if not hmac.compare_digest(supplied, csrf_token(principal.session_token)):
        raise HTTPException(status_code=status.HTTP_403_FORBIDDEN, detail="csrf_invalid")
