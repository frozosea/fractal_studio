"""T14 full browser happy path; no direct database manipulation."""

from __future__ import annotations

import os

import httpx
import pytest

from tests.e2e.release_helpers import (
    become_creator,
    create_published_listing,
    login,
    qr_png,
    register,
    release_api_url,
    settle_checkout,
    wait_for,
)


pytestmark = pytest.mark.skipif(not os.getenv("E2E_API_URL"), reason="set E2E_API_URL")


@pytest.mark.asyncio
async def test_full_mvp_happy_path() -> None:
    async with httpx.AsyncClient(base_url=release_api_url(), timeout=60, trust_env=False) as creator, \
            httpx.AsyncClient(base_url=release_api_url(), timeout=60, trust_env=False) as buyer, \
            httpx.AsyncClient(base_url=release_api_url(), timeout=60, trust_env=False) as operator:
        await register(creator, label="happy-creator")
        await become_creator(creator, label="happy")
        listing = await create_published_listing(creator, label="happy")
        await register(buyer, label="happy-buyer")
        payment = await settle_checkout(buyer=buyer, listing=listing)
        order = await wait_for(
            lambda: _order(buyer, payment["order"]["id"]),
            lambda value: value["data"]["status"] == "fulfilled",
        )
        assert order["data"]["amount"] == "19.90"
        download = await buyer.post(f"/v1/assets/{listing['assetId']}/download-url")
        assert download.status_code == 200 and download.json()["data"]["url"]

        payout = await creator.post(
            "/v1/me/payout-requests",
            headers={"Idempotency-Key": "happy-payout"},
            files={
                "amount": (None, "10.00"),
                "qrCode": ("qr.png", qr_png("happy"), "image/png"),
            },
        )
        assert payout.status_code == 201, payout.text
        payout_id = payout.json()["data"]["id"]
        await login(
            operator,
            email=os.environ["E2E_FINANCE_EMAIL"],
            password=os.environ["E2E_FINANCE_PASSWORD"],
        )
        pending = await operator.get("/internal/v1/payout-requests", params={"status": "pending"})
        assert pending.status_code == 200
        operator_row = next(item for item in pending.json()["data"] if item["id"] == payout_id)
        assert operator_row["qrUrl"] and operator_row["qrExpiresAt"]
        settled = await operator.post(
            f"/internal/v1/payout-requests/{payout_id}/mark-paid",
            headers={"Idempotency-Key": "happy-payout-paid"},
            json={"externalReference": "e2e-manual-transfer-001"},
        )
        assert settled.status_code == 200 and settled.json()["data"]["status"] == "paid"


async def _order(client: httpx.AsyncClient, order_id: str) -> dict[str, object]:
    response = await client.get(f"/v1/orders/{order_id}")
    assert response.status_code == 200, response.text
    return response.json()
