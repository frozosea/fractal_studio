"""Password-login disclosure boundaries."""

from __future__ import annotations

from contextlib import asynccontextmanager
from uuid import uuid4

import pytest
from fastapi import HTTPException, Request

from app.auth import service


class _Engine:
    def __init__(self, connection: object) -> None:
        self._connection = connection

    @asynccontextmanager
    async def begin(self):
        yield self._connection


def _request() -> Request:
    return Request({"type": "http", "method": "POST", "path": "/v1/auth/login", "headers": []})


@pytest.mark.asyncio
@pytest.mark.parametrize(
    ("password", "expected_detail"),
    [("wrong-password", "invalid_credentials"), ("correct-password", "account_disabled")],
)
async def test_disabled_account_is_only_disclosed_after_password_verification(
    monkeypatch: pytest.MonkeyPatch,
    password: str,
    expected_detail: str,
) -> None:
    user = {
        "id": uuid4(),
        "email": "disabled@example.test",
        "password_hash": "encoded-password",
        "status": "disabled",
    }

    async def find_by_email(_connection: object, _email: str) -> dict[str, object]:
        return user

    monkeypatch.setattr(service, "get_engine", lambda: _Engine(object()))
    monkeypatch.setattr(service.user_repository, "find_by_email", find_by_email)
    monkeypatch.setattr(
        service,
        "_verify_password",
        lambda candidate, encoded: candidate == "correct-password" and encoded == "encoded-password",
    )

    with pytest.raises(HTTPException) as raised:
        await service.login("disabled@example.test", password, _request())

    assert raised.value.status_code == 401
    assert raised.value.detail == expected_detail
