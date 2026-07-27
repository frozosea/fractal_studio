"""T14 authorization, validation and disclosure boundaries."""

from __future__ import annotations

import os

import httpx
import pytest

from tests.e2e.release_helpers import (
    assert_error,
    become_creator,
    create_published_listing,
    register,
    release_api_url,
)


pytestmark = pytest.mark.skipif(not os.getenv("E2E_API_URL"), reason="set E2E_API_URL")


@pytest.mark.asyncio
async def test_access_boundaries_and_safe_views() -> None:
    async with httpx.AsyncClient(base_url=release_api_url(), timeout=60, trust_env=False) as owner, \
            httpx.AsyncClient(base_url=release_api_url(), timeout=60, trust_env=False) as stranger:
        anonymous = await stranger.get("/v1/me")
        assert_error(anonymous, status=401, code="unauthenticated")
        await register(owner, label="security-owner")
        await become_creator(owner, label="security")
        listing = await create_published_listing(owner, label="security")
        asset = await owner.get(f"/v1/me/assets/{listing['assetId']}")
        assert asset.status_code == 200
        serialized = asset.text
        assert all(value not in serialized for value in ("objectKey", "computeRunId", "contentPath"))
        await register(stranger, label="security-stranger")
        other_asset = await stranger.get(f"/v1/me/assets/{listing['assetId']}")
        assert_error(other_asset, status=404, code="not_found")
        operator = await stranger.get("/internal/v1/payout-requests")
        assert_error(operator, status=403, code="forbidden")
        invalid_limit = await stranger.get("/v1/explore", params={"limit": 49})
        assert_error(invalid_limit, status=422, code="validation_error")
