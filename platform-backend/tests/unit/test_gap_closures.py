"""Regression checks for M1/M2 gap-closure invariants."""

from __future__ import annotations

import pytest
from pydantic import ValidationError

from app.core.config import Settings
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


def test_julia_recipe_requires_complete_complex_constant() -> None:
    with pytest.raises(ValidationError, match="juliaRe and juliaIm"):
        FractalSpec.model_validate({"version": 1, "julia": True, "juliaRe": 0.1})


def test_non_julia_recipe_keeps_optional_julia_constant_absent() -> None:
    assert FractalSpec.model_validate({"version": 1, "julia": False}).julia_re is None
