"""T14 common envelope/error matrix and public discovery contract."""

from __future__ import annotations

import os

import httpx
import pytest

from tests.e2e.release_helpers import CANONICAL_SPEC, assert_error, register, release_api_url


pytestmark = pytest.mark.skipif(not os.getenv("E2E_API_URL"), reason="set E2E_API_URL")


@pytest.mark.asyncio
async def test_public_contract_matrix_and_baseline_errors() -> None:
    async with httpx.AsyncClient(base_url=release_api_url(), timeout=30, trust_env=False) as client:
        explore = await client.get("/v1/explore", params={"sort": "newest", "limit": 24})
        assert explore.status_code == 200 and set(explore.json()) == {"data", "page"}
        missing = await client.get("/v1/listings/00000000-0000-0000-0000-000000000000")
        assert_error(missing, status=404, code="not_found")
        await register(client, label="contract")
        preview = await client.post(
            "/v1/studio/preview", json={"canonicalSpec": CANONICAL_SPEC, "width": 64, "height": 64}
        )
        assert preview.status_code == 200 and preview.headers["content-type"].startswith("image/png")
        rejected = await client.post(
            "/v1/studio/preview",
            json={"canonicalSpec": {**CANONICAL_SPEC, "variant": "compute_rejected"}, "width": 64, "height": 64},
        )
        assert_error(rejected, status=502, code="compute_error")
        invalid_webhook = await client.post(
            "/v1/webhooks/alipay", content="not=a&signed=form", headers={"Content-Type": "application/x-www-form-urlencoded"}
        )
        assert_error(invalid_webhook, status=422, code="validation_error")
