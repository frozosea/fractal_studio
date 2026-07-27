"""Opt-in M5 real HTTP/Compose checkout E2E."""

from __future__ import annotations

import os
import uuid

import httpx
import pytest
from sqlalchemy import text
from sqlalchemy.exc import DBAPIError
from sqlalchemy.ext.asyncio import create_async_engine

from tests.e2e.test_marketplace import _become_creator, _ready_image


pytestmark = pytest.mark.skipif(
    not (os.getenv("E2E_API_URL") and os.getenv("E2E_DATABASE_URL") and os.getenv("E2E_COMPUTE_AVAILABLE") == "1"),
    reason="set E2E_API_URL, E2E_DATABASE_URL and E2E_COMPUTE_AVAILABLE=1",
)


@pytest.mark.asyncio
async def test_checkout_creates_frozen_pending_order_and_never_settles(e2e_api_url: str) -> None:
    suffix = uuid.uuid4().hex[:10]
    async with httpx.AsyncClient(base_url=e2e_api_url, timeout=60, trust_env=False) as creator, httpx.AsyncClient(
        base_url=e2e_api_url, timeout=60, trust_env=False
    ) as buyer, httpx.AsyncClient(base_url=e2e_api_url, timeout=60, trust_env=False) as other:
        assert (await creator.post("/v1/auth/register", json={
            "email": f"checkout-creator-{suffix}@example.test", "password": "correct-horse-01"
        })).status_code == 201
        await _become_creator(creator, suffix=f"checkout_{suffix}", key="creator-profile")
        asset_id = await _ready_image(creator, seed=3001, key="checkout")
        draft = await creator.post("/v1/listings", headers={"Idempotency-Key": "checkout-listing"}, json={
            "assetId": asset_id, "title": "Checkout orbit", "description": "Frozen on checkout", "tags": ["checkout"],
            "price": "19.90", "licenceOffer": {"code": "personal", "termsVersion": "v1"},
        })
        assert draft.status_code == 201, draft.text
        listing = draft.json()["data"]
        listing_id, offer_id = listing["id"], listing["licenceOffer"]["id"]
        assert (await creator.post(f"/v1/listings/{listing_id}/publish", headers={"Idempotency-Key": "checkout-publish"})).status_code == 200

        buyer_registration = await buyer.post("/v1/auth/register", json={
            "email": f"checkout-buyer-{suffix}@example.test", "password": "correct-horse-01"
        })
        assert buyer_registration.status_code == 201
        payload = {"listingId": listing_id, "licenceOfferId": offer_id, "channel": "desktop_web"}
        created = await buyer.post("/v1/checkout", headers={"Idempotency-Key": "checkout-one"}, json=payload)
        assert created.status_code == 201, created.text
        data = created.json()["data"]
        order_id = data["order"]["id"]
        assert data["order"]["status"] == "pending_payment"
        assert data["order"]["amount"] == "19.90"
        assert data["paymentAttempt"]["status"] == "created"
        assert data["alipayForm"]["method"] == "POST"
        assert data["alipayForm"]["fields"]["method"] == "alipay.trade.page.pay"
        assert "private" not in str(data["alipayForm"]).lower()

        replay = await buyer.post("/v1/checkout", headers={"Idempotency-Key": "checkout-one"}, json=payload)
        assert replay.status_code == 201 and replay.json()["data"]["order"]["id"] == order_id
        assert (await buyer.post("/v1/checkout", headers={"Idempotency-Key": "checkout-tamper"}, json={**payload, "amount": "0.01"})).status_code == 422
        assert (await buyer.post("/v1/checkout", headers={"Idempotency-Key": "checkout-active"}, json=payload)).status_code == 409
        assert (await buyer.get(f"/v1/orders/{order_id}")).json()["data"]["id"] == order_id
        assert (await other.post("/v1/auth/register", json={
            "email": f"checkout-other-{suffix}@example.test", "password": "correct-horse-01"
        })).status_code == 201
        assert (await other.get(f"/v1/orders/{order_id}")).status_code == 404
        purchases = await buyer.get("/v1/me/purchases", params={"limit": 1})
        assert purchases.status_code == 200 and purchases.json()["data"][0]["id"] == order_id

        engine = create_async_engine(os.environ["E2E_DATABASE_URL"])
        try:
            async with engine.connect() as connection:
                item = (await connection.execute(text("""
                    SELECT oi.price_amount, oi.creator_amount, oi.platform_fee_amount, oi.listing_snapshot_json,
                           oi.licence_snapshot_json, o.status::text AS order_status
                    FROM order_items oi JOIN orders o ON o.id = oi.order_id
                    WHERE oi.order_id = CAST(:order_id AS uuid)
                """), {"order_id": order_id})).mappings().one()
                assert str(item["price_amount"]) == "19.90"
                assert item["creator_amount"] + item["platform_fee_amount"] == item["price_amount"]
                assert item["listing_snapshot_json"]["title"] == "Checkout orbit"
                assert item["licence_snapshot_json"]
                assert item["order_status"] == "pending_payment"
                assert await connection.scalar(text("SELECT count(*) FROM entitlements WHERE order_item_id = (SELECT id FROM order_items WHERE order_id = CAST(:order_id AS uuid))"), {"order_id": order_id}) == 0
                assert await connection.scalar(text("SELECT count(*) FROM ledger_entries WHERE order_item_id = (SELECT id FROM order_items WHERE order_id = CAST(:order_id AS uuid))"), {"order_id": order_id}) == 0
                assert await connection.scalar(text("SELECT count(*) FROM outbox_events WHERE aggregate_id = CAST(:order_id AS uuid) AND event_type = 'payment.reconcile.v1'"), {"order_id": order_id}) == 1
            with pytest.raises(DBAPIError):
                async with engine.begin() as connection:
                    await connection.execute(text("UPDATE order_items SET price_amount = 0.01 WHERE order_id = CAST(:order_id AS uuid)"), {"order_id": order_id})
        finally:
            await engine.dispose()

        assert (await creator.post(f"/v1/listings/{listing_id}/unpublish", headers={"Idempotency-Key": "checkout-unpublish"})).status_code == 200
        assert (await buyer.post("/v1/checkout", headers={"Idempotency-Key": "checkout-unpublished"}, json={
            "listingId": listing_id, "licenceOfferId": offer_id, "channel": "mobile_web"
        })).status_code == 404
