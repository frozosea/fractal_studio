"""Release journey: creator, buyer and finance operator complete whole browser flow."""

from __future__ import annotations

import os
import uuid
from urllib.parse import urlencode

import httpx
import pytest

from tests.e2e.release_helpers import (
    CANONICAL_SPEC,
    become_creator,
    create_ready_asset,
    login,
    qr_png,
    register,
    release_api_url,
    wait_for,
)


pytestmark = pytest.mark.skipif(not os.getenv("E2E_API_URL"), reason="set E2E_API_URL")


async def _order(client: httpx.AsyncClient, order_id: str) -> dict[str, object]:
    response = await client.get(f"/v1/orders/{order_id}")
    assert response.status_code == 200, response.text
    return response.json()


async def _settle(
    *, buyer: httpx.AsyncClient, listing: dict[str, object], trade_status: str = "TRADE_SUCCESS", refund_amount: str | None = None
) -> dict[str, object]:
    checkout = await buyer.post(
        "/v1/checkout",
        headers={"Idempotency-Key": f"full-checkout-{uuid.uuid4().hex}"},
        json={"listingId": listing["id"], "licenceOfferId": listing["licenceOffer"]["id"], "channel": "desktop_web"},
    )
    assert checkout.status_code == 201, checkout.text
    payment = checkout.json()["data"]
    notice_body: dict[str, object] = {
        "outTradeNo": payment["paymentAttempt"]["outTradeNo"],
        "tradeStatus": trade_status,
        "totalAmount": payment["order"]["amount"],
    }
    if refund_amount is not None:
        notice_body["refundAmount"] = refund_amount
    async with httpx.AsyncClient(base_url=os.environ["E2E_ALIPAY_STUB_URL"], timeout=15, trust_env=False) as stub:
        notice = await stub.post("/test/notifications", json=notice_body)
        assert notice.status_code == 200, notice.text
    webhook = await buyer.post(
        "/v1/webhooks/alipay",
        content=urlencode(notice.json()),
        headers={"Content-Type": "application/x-www-form-urlencoded"},
    )
    assert webhook.status_code == 200 and webhook.text == "success", webhook.text
    return payment


@pytest.mark.asyncio
async def test_creator_buyer_refund_and_payout_complete_journey() -> None:
    """One user journey, with a second sale after refund so funds can be paid out."""
    async with httpx.AsyncClient(base_url=release_api_url(), timeout=90, trust_env=False) as creator, \
            httpx.AsyncClient(base_url=release_api_url(), timeout=90, trust_env=False) as buyer, \
            httpx.AsyncClient(base_url=release_api_url(), timeout=90, trust_env=False) as operator:
        # Seller creates a visible, immutable work from a bounded preview and durable render.
        await register(creator, label="full-creator")
        await become_creator(creator, label="fullcreator")
        preview = await creator.post("/v1/studio/preview", json={"canonicalSpec": CANONICAL_SPEC, "width": 64, "height": 64})
        assert preview.status_code == 200 and preview.headers["content-type"].startswith("image/png")
        asset_id = await create_ready_asset(creator, label="full-journey")
        hidden = await creator.patch(
            f"/v1/me/assets/{asset_id}", headers={"Idempotency-Key": "full-asset-hide"}, json={"visibility": "hidden"}
        )
        assert hidden.status_code == 200 and hidden.json()["data"]["visibility"] == "hidden"
        restored = await creator.patch(
            f"/v1/me/assets/{asset_id}", headers={"Idempotency-Key": "full-asset-restore"}, json={"visibility": "private"}
        )
        assert restored.status_code == 200 and restored.json()["data"]["status"] == "ready"

        draft = await creator.post(
            "/v1/listings",
            headers={"Idempotency-Key": "full-listing-create"},
            json={
                "assetId": asset_id, "title": "Full journey fractal", "description": "first draft", "tags": ["e2e"],
                "price": "19.90", "licenceOffer": {"code": "personal", "termsVersion": "v1"},
            },
        )
        assert draft.status_code == 201, draft.text
        listing_id = draft.json()["data"]["id"]
        edited = await creator.patch(
            f"/v1/listings/{listing_id}", headers={"Idempotency-Key": "full-listing-edit"},
            json={"title": "Full journey fractal v1", "description": "published work", "tags": ["e2e", "fractal"]},
        )
        assert edited.status_code == 200, edited.text
        published = await creator.post(
            f"/v1/listings/{listing_id}/publish", headers={"Idempotency-Key": "full-listing-publish"}
        )
        assert published.status_code == 200, published.text
        listing = published.json()["data"]
        assert listing["status"] == "published" and listing["licenceOffer"]["id"]
        mine = await creator.get("/v1/me/listings", params={"status": "published"})
        assert mine.status_code == 200 and any(row["id"] == listing_id for row in mine.json()["data"])

        # Buyer discovers, bookmarks, purchases, receives entitlement and downloads private master.
        await register(buyer, label="full-buyer")
        explore = await buyer.get("/v1/explore", params={"q": "journey fractal", "limit": 24})
        assert explore.status_code == 200 and any(row["id"] == listing_id for row in explore.json()["data"])
        favorite = await buyer.post(f"/v1/assets/{asset_id}/favorite", headers={"Idempotency-Key": "full-favorite"})
        assert favorite.status_code == 201, favorite.text
        favorites = await buyer.get("/v1/me/favorites")
        assert favorites.status_code == 200 and any(row["assetId"] == asset_id for row in favorites.json()["data"])

        first_sale = await _settle(buyer=buyer, listing=listing)
        fulfilled = await wait_for(
            lambda: _order(buyer, first_sale["order"]["id"]), lambda row: row["data"]["status"] == "fulfilled"
        )
        assert fulfilled["data"]["amount"] == "19.90"
        download = await buyer.post(f"/v1/assets/{asset_id}/download-url")
        assert download.status_code == 200 and download.json()["data"]["url"]
        purchases = await buyer.get("/v1/me/purchases")
        assert purchases.status_code == 200 and any(row["id"] == first_sale["order"]["id"] for row in purchases.json()["data"])

        # A return/refund is authoritative payment-provider state, never a browser trust action.
        async with httpx.AsyncClient(base_url=os.environ["E2E_ALIPAY_STUB_URL"], timeout=15, trust_env=False) as stub:
            refund_notice = await stub.post(
                "/test/notifications",
                json={
                    "outTradeNo": first_sale["paymentAttempt"]["outTradeNo"], "tradeStatus": "TRADE_CLOSED",
                    "totalAmount": "19.90", "refundAmount": "19.90",
                },
            )
            assert refund_notice.status_code == 200, refund_notice.text
        refunded = await buyer.post(
            "/v1/webhooks/alipay", content=urlencode(refund_notice.json()),
            headers={"Content-Type": "application/x-www-form-urlencoded"},
        )
        assert refunded.status_code == 200 and refunded.text == "success"
        reversed_order = await wait_for(
            lambda: _order(buyer, first_sale["order"]["id"]), lambda row: row["data"]["status"] == "payment_exception"
        )
        assert reversed_order["data"]["status"] == "payment_exception"
        assert (await buyer.post(f"/v1/assets/{asset_id}/download-url")).status_code == 403
        unfavorite = await buyer.delete(f"/v1/assets/{asset_id}/favorite", headers={"Idempotency-Key": "full-unfavorite"})
        assert unfavorite.status_code == 204

        # A second paid purchase creates available creator balance; operator then settles manual payout.
        second_sale = await _settle(buyer=buyer, listing=listing)
        await wait_for(lambda: _order(buyer, second_sale["order"]["id"]), lambda row: row["data"]["status"] == "fulfilled")
        payout = await creator.post(
            "/v1/me/payout-requests", headers={"Idempotency-Key": "full-payout"},
            files={"amount": (None, "10.00"), "qrCode": ("payout.png", qr_png("full-journey"), "image/png")},
        )
        assert payout.status_code == 201, payout.text
        payout_id = payout.json()["data"]["id"]
        assert "qrUrl" not in payout.text and "externalReference" not in payout.text
        await login(operator, email=os.environ["E2E_FINANCE_EMAIL"], password=os.environ["E2E_FINANCE_PASSWORD"])
        queue = await operator.get("/internal/v1/payout-requests", params={"status": "pending"})
        assert queue.status_code == 200
        operator_row = next(row for row in queue.json()["data"] if row["id"] == payout_id)
        assert operator_row["qrUrl"] and operator_row["qrExpiresAt"]
        paid = await operator.post(
            f"/internal/v1/payout-requests/{payout_id}/mark-paid",
            headers={"Idempotency-Key": f"full-payout-paid-{uuid.uuid4().hex}"},
            json={"externalReference": "full-journey-transfer-001"},
        )
        assert paid.status_code == 200 and paid.json()["data"]["status"] == "paid"
