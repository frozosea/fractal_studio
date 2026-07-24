"""Small request-local context used by audit and logs."""

from __future__ import annotations

from contextvars import ContextVar

from fastapi import Request


request_id_var: ContextVar[str | None] = ContextVar("request_id", default=None)
user_id_var: ContextVar[str | None] = ContextVar("user_id", default=None)
idempotency_key_var: ContextVar[str | None] = ContextVar("idempotency_key", default=None)


def request_id(request: Request) -> str:
    """Return boundary-assigned ID without accepting user-controlled audit data."""
    return getattr(request.state, "request_id", request.headers.get("x-request-id", "unknown"))
