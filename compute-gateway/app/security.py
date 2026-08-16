"""Private service-key dependencies."""

from __future__ import annotations

import secrets

from fastapi import Header

from app.config import get_settings
from app.errors import GatewayError

settings = get_settings()


def _require(authorization: str | None, expected: str) -> None:
    # compare_digest on str rejects non-ASCII input with TypeError; encode to
    # bytes so an attacker-supplied header can only ever yield 401, never 500.
    supplied = (authorization or "").encode("latin-1")
    if not supplied or not secrets.compare_digest(supplied, f"Bearer {expected}".encode("latin-1")):
        raise GatewayError(401, "COMPUTE_UNAUTHORIZED", "valid Compute service credential required")


def require_gateway_key(authorization: str | None = Header(default=None)) -> None:
    _require(authorization, settings.compute_gateway_service_key)


def require_admin_key(authorization: str | None = Header(default=None)) -> None:
    _require(authorization, settings.compute_gateway_admin_key)
