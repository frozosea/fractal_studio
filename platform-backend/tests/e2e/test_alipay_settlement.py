"""Opt-in T12 real HTTP, PostgreSQL, outbox-worker and RSA2 Alipay-stub E2E."""

from __future__ import annotations

import asyncio
import os
import uuid

import httpx
import pytest
from sqlalchemy import text
from sqlalchemy.ext.asyncio import create_async_engine

from tests.e2e.test_marketplace import _become_creator, _ready_image


pytestmark = pytest.mark.skipif(
    not (os.getenv("E2E_API_URL") and os.getenv("E2E_DATABASE_URL") and os.getenv("E2E_ALIPAY_STUB_URL")
            and os.getenv("E2E_COMPUTE_AVAILABLE") == "1" and os.getenv("E2E_PLATFORM_WORKER") == "1"),
    reason="set Compose API/DB/worker/Compute variables and E2E_ALIPAY_STUB_URL",
)


async def _notification(stub: httpx.AsyncClient, **body: object) -> dict[str, str]:
    response = await stub.post("/test/notifications", json=body)
    assert response.status_code == 200, response.text
    return response.json()


async def _wait_order(client: httpx.AsyncClient, order_id: str, expected_status: str) -> dict[str, object]:
    for _ in range(45):
        response = await client.get(f"/v1/orders/{order_id}")
        assert response.status_code == 200, response.text
        if response.json()["data"]["status"] == expected_status:
            return response.json()["data"]
        await asyncio.sleep(1)
    raise AssertionError(f"order {order_id} did not reach {expected_status}")


async def _requeue_reconcile(payment_attempt_id: str) -> None:
    engine = create_async_engine(os.environ["E2E_DATABASE_URL"])
    try:
        async with engine.begin() as connection:
            await connection.execute(text("""
                UPDATE outbox_events SET status = 'pending', available_at = now(), completed_at = NULL,
                    lease_owner = NULL, lease_until = NULL
                WHERE event_type = 'payment.reconcile.v1'
                  AND payload_json->>'paymentAttemptId' = :payment_attempt_id
            """), {"payment_attempt_id": payment_attempt_id})
    finally:
        await engine.dispose()


@pytest.mark.asyncio
async def test_signed_notification_reconcile_close_and_reversal(e2e_api_url: str) -> None:
    suffix = uuid.uuid4().hex[:10]
    stub_url = os.environ["E2E_ALIPAY_STUB_URL"]
    async with httpx.AsyncClient(base_url=e2e_api_url, timeout=60, trust_env=False) as creator, httpx.AsyncClient(
        base_url=e2e_api_url, timeout=60, trust_env=False
    ) as buyer, httpx.AsyncClient(base_url=e2e_api_url, timeout=60, trust_env=False) as buyer_two, httpx.AsyncClient(
        base_url=e2e_api_url, timeout=60, trust_env=False
    ) as buyer_three, httpx.AsyncClient(base_url=stub_url, timeout=20, trust_env=False) as stub:
        assert (await creator.post("/v1/auth/register", json={
            "email": f"alipay-creator-{suffix}@example.test", "password": "correct-horse-01"
        })).status_code == 201
        await _become_creator(creator, suffix=f"alipay_{suffix}", key="alipay-profile")
        asset_id = await _ready_image(creator, seed=4011, key="alipay")
        listing = await creator.post("/v1/listings", headers={"Idempotency-Key": "alipay-listing"}, json={
            "assetId": asset_id, "title": "Alipay orbit", "description": "", "tags": ["alipay"], "price": "19.90",
            "licenceOffer": {"code": "personal", "termsVersion": "v1"},
        })
        assert listing.status_code == 201, listing.text
        listing_data = listing.json()["data"]
        assert (await creator.post(f"/v1/listings/{listing_data['id']}/publish", headers={"Idempotency-Key": "alipay-publish"})).status_code == 200

        assert (await buyer.post("/v1/auth/register", json={
            "email": f"alipay-buyer-{suffix}@example.test", "password": "correct-horse-01"
        })).status_code == 201
        checkout_payload = {"listingId": listing_data["id"], "licenceOfferId": listing_data["licenceOffer"]["id"], "channel": "desktop_web"}
        checkout = await buyer.post("/v1/checkout", headers={"Idempotency-Key": "alipay-one"}, json=checkout_payload)
        assert checkout.status_code == 201, checkout.text
        payment = checkout.json()["data"]["paymentAttempt"]
        order_id, attempt_id, out_trade_no = checkout.json()["data"]["order"]["id"], payment["id"], payment["outTradeNo"]

        invalid = await _notification(stub, outTradeNo=out_trade_no, totalAmount="19.90", tradeStatus="TRADE_SUCCESS")
        invalid["sign"] = "invalid"
        assert (await buyer.post("/v1/webhooks/alipay", data=invalid)).status_code == 422
        wrong_merchant = await _notification(stub, outTradeNo=out_trade_no, appId="wrong-app", totalAmount="19.90", tradeStatus="TRADE_SUCCESS")
        assert (await buyer.post("/v1/webhooks/alipay", data=wrong_merchant)).status_code == 403
        wrong_amount = await _notification(stub, outTradeNo=out_trade_no, totalAmount="0.01", tradeStatus="TRADE_SUCCESS")
        assert (await buyer.post("/v1/webhooks/alipay", data=wrong_amount)).status_code == 409

        signed = await _notification(stub, outTradeNo=out_trade_no, totalAmount="19.90", tradeStatus="TRADE_SUCCESS")
        webhook = await buyer.post("/v1/webhooks/alipay", data=signed)
        assert webhook.status_code == 200 and webhook.text == "success"
        assert (await buyer.post("/v1/webhooks/alipay", data=signed)).text == "success"
        assert (await _wait_order(buyer, order_id, "fulfilled"))["paidAt"]
        assert (await buyer.post(f"/v1/assets/{asset_id}/download-url")).status_code == 200

        engine = create_async_engine(os.environ["E2E_DATABASE_URL"])
        try:
            async with engine.connect() as connection:
                assert await connection.scalar(text("SELECT count(*) FROM entitlements WHERE order_item_id = (SELECT id FROM order_items WHERE order_id = CAST(:order_id AS uuid)) AND status = 'active'"), {"order_id": order_id}) == 1
                assert await connection.scalar(text("SELECT count(*) FROM ledger_entries WHERE order_item_id = (SELECT id FROM order_items WHERE order_id = CAST(:order_id AS uuid))"), {"order_id": order_id}) == 2
                assert await connection.scalar(text("SELECT count(*) FROM payment_notifications WHERE payment_attempt_id = CAST(:attempt_id AS uuid)"), {"attempt_id": attempt_id}) == 1
        finally:
            await engine.dispose()

        reversal_notice = await _notification(
            stub, outTradeNo=out_trade_no, totalAmount="19.90", tradeStatus="TRADE_CLOSED", refundAmount="19.90"
        )
        assert (await buyer.post("/v1/webhooks/alipay", data=reversal_notice)).text == "success"
        assert (await _wait_order(buyer, order_id, "payment_exception"))["status"] == "payment_exception"
        assert (await buyer.post(f"/v1/assets/{asset_id}/download-url")).status_code == 403

        assert (await buyer_two.post("/v1/auth/register", json={
            "email": f"alipay-buyer-two-{suffix}@example.test", "password": "correct-horse-01"
        })).status_code == 201
        pending = await buyer_two.post("/v1/checkout", headers={"Idempotency-Key": "alipay-two"}, json=checkout_payload)
        assert pending.status_code == 201, pending.text
        pending_data = pending.json()["data"]
        await stub.put(f"/test/trades/{pending_data['paymentAttempt']['outTradeNo']}", json={"tradeStatus": "TRADE_SUCCESS", "totalAmount": "19.90"})
        assert (await _wait_order(buyer_two, pending_data["order"]["id"], "fulfilled"))["status"] == "fulfilled"

        assert (await buyer_three.post("/v1/auth/register", json={
            "email": f"alipay-buyer-three-{suffix}@example.test", "password": "correct-horse-01"
        })).status_code == 201
        close_pending = await buyer_three.post("/v1/checkout", headers={"Idempotency-Key": "alipay-three"}, json=checkout_payload)
        assert close_pending.status_code == 201
        close_data = close_pending.json()["data"]
        await stub.put(
            f"/test/trades/{close_data['paymentAttempt']['outTradeNo']}",
            json={"tradeStatus": "WAIT_BUYER_PAY", "totalAmount": "19.90"},
        )
        engine = create_async_engine(os.environ["E2E_DATABASE_URL"])
        try:
            async with engine.begin() as connection:
                await connection.execute(text("UPDATE payment_attempts SET expires_at = now() - interval '1 minute' WHERE id = CAST(:id AS uuid)"), {"id": close_data["paymentAttempt"]["id"]})
        finally:
            await engine.dispose()
        await _requeue_reconcile(close_data["paymentAttempt"]["id"])
        assert (await _wait_order(buyer_three, close_data["order"]["id"], "closed"))["status"] == "closed"

        partial = await buyer_three.post("/v1/checkout", headers={"Idempotency-Key": "alipay-four"}, json=checkout_payload)
        assert partial.status_code == 201
        partial_data = partial.json()["data"]
        await stub.put(
            f"/test/trades/{partial_data['paymentAttempt']['outTradeNo']}",
            json={"tradeStatus": "TRADE_SUCCESS", "totalAmount": "19.90"},
        )
        assert (await _wait_order(buyer_three, partial_data["order"]["id"], "fulfilled"))["status"] == "fulfilled"
        await stub.put(
            f"/test/trades/{partial_data['paymentAttempt']['outTradeNo']}",
            json={"tradeStatus": "TRADE_CLOSED", "totalAmount": "19.90", "refundAmount": "1.00"},
        )
        await _requeue_reconcile(partial_data["paymentAttempt"]["id"])
        assert (await _wait_order(buyer_three, partial_data["order"]["id"], "payment_exception"))["status"] == "payment_exception"

        paid_out = await buyer_three.post("/v1/checkout", headers={"Idempotency-Key": "alipay-five"}, json=checkout_payload)
        assert paid_out.status_code == 201
        paid_out_data = paid_out.json()["data"]
        await stub.put(
            f"/test/trades/{paid_out_data['paymentAttempt']['outTradeNo']}",
            json={"tradeStatus": "TRADE_SUCCESS", "totalAmount": "19.90"},
        )
        assert (await _wait_order(buyer_three, paid_out_data["order"]["id"], "fulfilled"))["status"] == "fulfilled"
        engine = create_async_engine(os.environ["E2E_DATABASE_URL"])
        try:
            async with engine.begin() as connection:
                await connection.execute(text("""
                    UPDATE creator_balances SET available_amount = 0
                    WHERE creator_id = (SELECT creator_id FROM order_items WHERE order_id = CAST(:order_id AS uuid))
                """), {"order_id": paid_out_data["order"]["id"]})
        finally:
            await engine.dispose()
        await stub.put(
            f"/test/trades/{paid_out_data['paymentAttempt']['outTradeNo']}",
            json={"tradeStatus": "TRADE_CLOSED", "totalAmount": "19.90", "refundAmount": "19.90"},
        )
        await _requeue_reconcile(paid_out_data["paymentAttempt"]["id"])
        assert (await _wait_order(buyer_three, paid_out_data["order"]["id"], "payment_exception"))["status"] == "payment_exception"
        engine = create_async_engine(os.environ["E2E_DATABASE_URL"])
        try:
            async with engine.connect() as connection:
                for target in (partial_data, paid_out_data):
                    assert await connection.scalar(text("""
                        SELECT status::text FROM refund_reversals
                        WHERE payment_attempt_id = CAST(:attempt_id AS uuid)
                    """), {"attempt_id": target["paymentAttempt"]["id"]}) == "manual_review"
                    assert await connection.scalar(text("""
                        SELECT count(*) FROM entitlements WHERE order_item_id = (
                          SELECT id FROM order_items WHERE order_id = CAST(:order_id AS uuid)
                        ) AND status = 'active'
                    """), {"order_id": target["order"]["id"]}) == 1
        finally:
            await engine.dispose()
