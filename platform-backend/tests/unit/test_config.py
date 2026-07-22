from __future__ import annotations

import pytest

from app.core.config import get_settings


def test_production_rejects_foundation_routes(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("APP_ENV", "production")
    monkeypatch.setenv("FOUNDATION_ROUTES_ENABLED", "1")
    get_settings.cache_clear()
    with pytest.raises(RuntimeError, match="must be false"):
        get_settings()
    get_settings.cache_clear()

