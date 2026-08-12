"""Regression checks for M1/M2 gap-closure invariants."""

from __future__ import annotations

import io

import pytest
from fastapi import HTTPException
from pydantic import ValidationError
import qrcode
from starlette.datastructures import Headers, UploadFile
from starlette.requests import Request

from app.core import access_middleware
from app.core.config import Settings
from app.finance.manual_payout_service import ManualPayoutService
from app.studio.models import FractalSpec


def test_production_settings_reject_insecure_session_cookie() -> None:
    with pytest.raises(ValidationError, match="SESSION_COOKIE_SECURE"):
        Settings(
            app_env="production",
            database_url="postgresql+asyncpg://unused",
            session_secret="a-non-development-secret-with-at-least-32-characters",
            api_origin="https://platform.example.test",
            cors_origins="https://platform.example.test",
            session_cookie_secure=False,
        )


def _origin_request(origin: str | None) -> Request:
    headers = [] if origin is None else [(b"origin", origin.encode())]
    return Request({"type": "http", "method": "POST", "path": "/v1/auth/login", "headers": headers})


def test_login_accepts_each_configured_first_party_origin(monkeypatch: pytest.MonkeyPatch) -> None:
    settings = Settings(
        database_url="postgresql+asyncpg://unused",
        session_secret="x" * 32,
        api_origin="https://fractal.example.test",
        cors_origins="https://fractal.example.test,http://localhost:3010",
    )
    monkeypatch.setattr(access_middleware, "get_settings", lambda: settings)

    access_middleware.enforce_same_origin_or_no_origin(
        _origin_request("https://fractal.example.test")
    )
    access_middleware.enforce_same_origin_or_no_origin(_origin_request("http://localhost:3010"))
    access_middleware.enforce_same_origin_or_no_origin(_origin_request(None))


def test_login_rejects_unconfigured_origin(monkeypatch: pytest.MonkeyPatch) -> None:
    settings = Settings(
        database_url="postgresql+asyncpg://unused",
        session_secret="x" * 32,
        api_origin="https://fractal.example.test",
        cors_origins="http://localhost:3010",
    )
    monkeypatch.setattr(access_middleware, "get_settings", lambda: settings)

    with pytest.raises(HTTPException, match="untrusted_origin"):
        access_middleware.enforce_same_origin_or_no_origin(_origin_request("https://evil.test"))


def test_julia_recipe_requires_complete_complex_constant() -> None:
    with pytest.raises(ValidationError, match="juliaRe and juliaIm"):
        FractalSpec.model_validate({"version": 1, "julia": True, "juliaRe": 0.1})


def test_non_julia_recipe_keeps_optional_julia_constant_absent() -> None:
    assert FractalSpec.model_validate({"version": 1, "julia": False}).julia_re is None


@pytest.mark.asyncio
async def test_payout_qr_must_contain_a_decodable_qr_code() -> None:
    service = ManualPayoutService(
        storage=object(),  # type: ignore[arg-type]
        settings=Settings(database_url="postgresql+asyncpg://unused", session_secret="x" * 32),
    )
    image = qrcode.make("https://payout.example.test/creator")
    encoded = io.BytesIO()
    image.save(encoded, format="PNG")
    valid = UploadFile(file=io.BytesIO(encoded.getvalue()), headers=Headers({"content-type": "image/png"}))
    evidence = await service.validate_qr_upload(valid)
    assert evidence.media_type == "image/png" and evidence.sanitized_bytes

    from PIL import Image

    blank = io.BytesIO()
    Image.new("RGB", (64, 64), "white").save(blank, format="PNG")
    invalid = UploadFile(file=io.BytesIO(blank.getvalue()), headers=Headers({"content-type": "image/png"}))
    with pytest.raises(HTTPException, match="invalid_qr_image"):
        await service.validate_qr_upload(invalid)
