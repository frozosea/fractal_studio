"""Private service-key dependencies."""

from __future__ import annotations

import secrets

from fastapi import Header

from app.config import get_settings
from app.errors import GatewayError

settings = get_settings()


def _require(authorization: str | None, expected: str) -> None:
    if not authorization or not secrets.compare_digest(authorization, f"Bearer {expected}"):
        raise GatewayError(401, "COMPUTE_UNAUTHORIZED", "valid Compute service credential required")


def require_gateway_key(authorization: str | None = Header(default=None)) -> None:
    _require(authorization, settings.compute_gateway_service_key)


def require_admin_key(authorization: str | None = Header(default=None)) -> None:
    _require(authorization, settings.compute_gateway_admin_key)
