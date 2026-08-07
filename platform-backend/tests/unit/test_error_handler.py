import asyncio
import os

os.environ.setdefault("DATABASE_URL", "postgresql+asyncpg://test:test@localhost:5432/test")
os.environ.setdefault("SESSION_SECRET", "test-session-secret")

from fastapi import HTTPException
from starlette.requests import Request

from app.main import platform_http_exception_handler


def test_registration_conflict_keeps_public_domain_code() -> None:
    request = Request({"type": "http", "method": "POST", "path": "/v1/auth/register", "headers": []})

    response = asyncio.run(
        platform_http_exception_handler(
            request,
            HTTPException(status_code=409, detail="email_already_registered"),
        )
    )

    assert response.status_code == 409
    assert response.body == (
        b'{"error":{"code":"email_already_registered","message":"email already registered","details":{}}}'
    )


def test_compute_capacity_error_keeps_explicit_public_code() -> None:
    request = Request({"type": "http", "method": "POST", "path": "/v1/render-jobs", "headers": []})

    response = asyncio.run(
        platform_http_exception_handler(
            request,
            HTTPException(status_code=503, detail="COMPUTE_CAPACITY_EXHAUSTED"),
        )
    )

    assert response.status_code == 503
    assert response.body == (
        b'{"error":{"code":"COMPUTE_CAPACITY_EXHAUSTED",'
        b'"message":"COMPUTE CAPACITY EXHAUSTED","details":{}}}'
    )
