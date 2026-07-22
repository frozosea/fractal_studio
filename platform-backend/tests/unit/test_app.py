from __future__ import annotations

import httpx
import pytest

from app.main import app


@pytest.mark.asyncio
async def test_health_and_request_id() -> None:
    async with httpx.AsyncClient(
        transport=httpx.ASGITransport(app=app), base_url="http://platform.test"
    ) as client:
        response = await client.get("/health/live", headers={"X-Request-Id": "known-request"})
    assert response.status_code == 200
    assert response.json()["service"] == "fractal-studio-platform"
    assert response.headers["X-Request-Id"] == "known-request"


def test_foundation_schema_contains_transactional_tables() -> None:
    from app.core.db import Base
    from app.outbox import models as outbox_models  # noqa: F401
    from app.studio import models as studio_models  # noqa: F401

    assert {"render_jobs", "quota_reservations", "outbox_events"} <= set(Base.metadata.tables)
    assert "idempotency_key" in Base.metadata.tables["render_jobs"].c
    assert "lease_until" in Base.metadata.tables["outbox_events"].c

