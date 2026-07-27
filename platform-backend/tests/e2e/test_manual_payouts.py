"""Opt-in M6b API/DB/MinIO/worker proof for private manual creator payouts."""

from __future__ import annotations

import asyncio
import io
import os
import uuid

import httpx
import pytest
import qrcode
from sqlalchemy import text
from sqlalchemy.ext.asyncio import create_async_engine

from tests.e2e.test_alipay_settlement import _wait_order
from tests.e2e.test_marketplace import _become_creator, _ready_image


pytestmark = pytest.mark.skipif(
    not (os.getenv("E2E_API_URL") and os.getenv("E2E_DATABASE_URL") and os.getenv("E2E_ALIPAY_STUB_URL")
         and os.getenv("E2E_COMPUTE_AVAILABLE") == "1" and os.getenv("E2E_PLATFORM_WORKER") == "1"),
    reason="set Compose API/DB/worker/Compute and Alipay-stub E2E variables",
)


def _qr_bytes(color: tuple[int, int, int], image_format: str = "PNG") -> bytes:
    image = qrcode.make(f"https://qr.example.test/{color[0]}-{color[1]}-{color[2]}")
    if image_format == "JPEG":
        image = image.convert("RGB")
    output = io.BytesIO()
    image.save(output, format=image_format)
    return output.getvalue()


async def _grant_operator(user_id: str) -> None:
    engine = create_async_engine(os.environ["E2E_DATABASE_URL"])
    try:
        async with engine.begin() as connection:
            await connection.execute(text("""INSERT INTO user_roles (user_id, role)
                VALUES (CAST(:user_id AS uuid), 'finance_operator') ON CONFLICT DO NOTHING"""), {"user_id": user_id})
    finally:
        await engine.dispose()


async def _seed_creator_balance(
    creator: httpx.AsyncClient, buyer: httpx.AsyncClient, stub: httpx.AsyncClient, suffix: str
) -> None:
    await _become_creator(creator, suffix=f"payout_{suffix}", key="payout-profile")
    asset_id = await _ready_image(creator, seed=6013, key="payout")
    listing = await creator.post("/v1/listings", headers={"Idempotency-Key": "payout-listing"}, json={
        "assetId": asset_id, "title": "Payout balance", "description": "", "tags": ["payout"],
        "price": "19.90", "licenceOffer": {"code": "personal", "termsVersion": "v1"},
    })
    assert listing.status_code == 201, listing.text
    data = listing.json()["data"]
    assert (await creator.post(f"/v1/listings/{data['id']}/publish", headers={"Idempotency-Key": "payout-publish"})).status_code == 200
    checkout = await buyer.post("/v1/checkout", headers={"Idempotency-Key": "payout-checkout"}, json={
        "listingId": data["id"], "licenceOfferId": data["licenceOffer"]["id"], "channel": "desktop_web",
    })
    assert checkout.status_code == 201, checkout.text
    paid = checkout.json()["data"]
    assert (await stub.put(f"/test/trades/{paid['paymentAttempt']['outTradeNo']}", json={
        "tradeStatus": "TRADE_SUCCESS", "totalAmount": "19.90"
    })).status_code == 200
    assert (await _wait_order(buyer, paid["order"]["id"], "fulfilled"))["status"] == "fulfilled"


@pytest.mark.asyncio
async def test_private_manual_payout_reserve_settle_release_and_cleanup(e2e_api_url: str) -> None:
    suffix = uuid.uuid4().hex[:10]
    async with httpx.AsyncClient(base_url=e2e_api_url, timeout=60, trust_env=False) as creator, \
            httpx.AsyncClient(base_url=e2e_api_url, timeout=60, trust_env=False) as buyer, \
            httpx.AsyncClient(base_url=e2e_api_url, timeout=60, trust_env=False) as operator, \
            httpx.AsyncClient(base_url=os.environ["E2E_ALIPAY_STUB_URL"], timeout=30, trust_env=False) as stub:
        creator_registered = await creator.post("/v1/auth/register", json={
            "email": f"payout-creator-{suffix}@example.test", "password": "correct-horse-01"
        })
        assert creator_registered.status_code == 201, creator_registered.text
        assert (await buyer.post("/v1/auth/register", json={
            "email": f"payout-buyer-{suffix}@example.test", "password": "correct-horse-01"
        })).status_code == 201
        operator_registered = await operator.post("/v1/auth/register", json={
            "email": f"payout-operator-{suffix}@example.test", "password": "correct-horse-01"
        })
        assert operator_registered.status_code == 201
        await _grant_operator(operator_registered.json()["data"]["id"])
        await _seed_creator_balance(creator, buyer, stub, suffix)

        bad_mime = await creator.post("/v1/me/payout-requests", headers={"Idempotency-Key": "payout-bad-mime"}, files={
            "amount": (None, "10.00"), "qrCode": ("evidence.gif", _qr_bytes((1, 2, 3)), "image/gif"),
        })
        assert bad_mime.status_code == 422
        too_large = await creator.post("/v1/me/payout-requests", headers={"Idempotency-Key": "payout-large"}, files={
            "amount": (None, "10.00"), "qrCode": ("large.png", b"x" * (2 * 1024 * 1024 + 1), "image/png"),
        })
        assert too_large.status_code == 413

        qr_one = _qr_bytes((10, 20, 30))
        create = await creator.post("/v1/me/payout-requests", headers={"Idempotency-Key": "payout-create-one"}, files={
            "amount": (None, "10.00"), "qrCode": ("qr.png", qr_one, "image/png"),
        })
        assert create.status_code == 201, create.text
        payout_one = create.json()["data"]
        assert {"qrObjectKey", "qrUrl", "externalReference"}.isdisjoint(payout_one)
        assert (await creator.get("/v1/me/payout-requests")).json()["data"][0]["id"] == payout_one["id"]
        changed_replay = await creator.post("/v1/me/payout-requests", headers={"Idempotency-Key": "payout-create-one"}, files={
            "amount": (None, "10.00"), "qrCode": ("qr.png", _qr_bytes((30, 20, 10)), "image/png"),
        })
        assert changed_replay.status_code == 409
        assert (await creator.post("/v1/me/payout-requests", headers={"Idempotency-Key": "payout-second-pending"}, files={
            "amount": (None, "1.00"), "qrCode": ("qr.png", _qr_bytes((1, 1, 1)), "image/png"),
        })).status_code == 409
        assert (await creator.post("/v1/me/payout-requests", headers={"Idempotency-Key": "payout-insufficient"}, files={
            "amount": (None, "999.00"), "qrCode": ("qr.png", _qr_bytes((2, 2, 2)), "image/png"),
        })).status_code == 409
        assert (await buyer.get("/internal/v1/payout-requests")).status_code == 403
        internal = await operator.get("/internal/v1/payout-requests", params={"status": "pending"})
        assert internal.status_code == 200, internal.text
        operator_view = next(row for row in internal.json()["data"] if row["id"] == payout_one["id"])
        assert operator_view["qrUrl"] and operator_view["qrExpiresAt"]
        assert (await buyer.get(f"/v1/payout-qr/{payout_one['id']}")).status_code == 404

        paid = await operator.post(f"/internal/v1/payout-requests/{payout_one['id']}/mark-paid", headers={
            "Idempotency-Key": "payout-paid-one"}, json={"externalReference": "merchant-transfer-001"})
        assert paid.status_code == 200, paid.text
        assert paid.json()["data"]["status"] == "paid" and "externalReference" not in paid.text
        assert (await operator.post(f"/internal/v1/payout-requests/{payout_one['id']}/mark-paid", headers={
            "Idempotency-Key": "payout-paid-one"}, json={"externalReference": "merchant-transfer-001"})).status_code == 200
        assert (await creator.post(f"/v1/me/payout-requests/{payout_one['id']}/cancel", headers={
            "Idempotency-Key": "payout-cancel-paid"})).status_code == 409

        create_two = await creator.post("/v1/me/payout-requests", headers={"Idempotency-Key": "payout-create-two"}, files={
            "amount": (None, "5.00"), "qrCode": ("qr.jpg", _qr_bytes((40, 50, 60), "JPEG"), "image/jpeg"),
        })
        assert create_two.status_code == 201, create_two.text
        payout_two = create_two.json()["data"]
        rejected = await operator.post(f"/internal/v1/payout-requests/{payout_two['id']}/reject", headers={
            "Idempotency-Key": "payout-reject-two"}, json={"reason": "invalid account evidence"})
        assert rejected.status_code == 200 and rejected.json()["data"]["status"] == "rejected"
        assert (await operator.post(f"/internal/v1/payout-requests/{payout_two['id']}/reject", headers={
            "Idempotency-Key": "payout-reject-two"}, json={"reason": "invalid account evidence"})).status_code == 200

        create_three = await creator.post("/v1/me/payout-requests", headers={"Idempotency-Key": "payout-create-three"}, files={
            "amount": (None, "1.00"), "qrCode": ("qr.png", _qr_bytes((70, 80, 90)), "image/png"),
        })
        assert create_three.status_code == 201, create_three.text
        payout_three = create_three.json()["data"]
        cancelled = await creator.post(f"/v1/me/payout-requests/{payout_three['id']}/cancel", headers={
            "Idempotency-Key": "payout-cancel-three"})
        assert cancelled.status_code == 200 and cancelled.json()["data"]["status"] == "cancelled"
        assert (await creator.post(f"/v1/me/payout-requests/{payout_three['id']}/cancel", headers={
            "Idempotency-Key": "payout-cancel-three"})).status_code == 200

        engine = create_async_engine(os.environ["E2E_DATABASE_URL"])
        try:
            async with engine.begin() as connection:
                balance = await connection.execute(text("""SELECT available_amount, reserved_amount FROM creator_balances
                    WHERE creator_id = CAST(:creator_id AS uuid)"""), {"creator_id": creator_registered.json()["data"]["id"]})
                assert tuple(str(value) for value in balance.one()) == ("5.92", "0.00")
                entries = await connection.execute(text("""SELECT entry_type::text FROM ledger_entries
                    WHERE payout_request_id IN (CAST(:one AS uuid), CAST(:two AS uuid), CAST(:three AS uuid))
                    ORDER BY entry_type"""), {"one": payout_one["id"], "two": payout_two["id"], "three": payout_three["id"]})
                assert [row[0] for row in entries] == [
                    "payout_paid", "payout_released", "payout_released",
                    "payout_reserved", "payout_reserved", "payout_reserved",
                ]
                audit = await connection.scalar(text("""SELECT metadata_json FROM audit_events
                    WHERE subject_id = CAST(:id AS uuid) AND action = 'payout.rejected'"""), {"id": payout_two["id"]})
                assert audit["requestId"] and "invalid account evidence" not in str(audit)
                await connection.execute(text("""UPDATE outbox_events SET available_at = now()
                    WHERE event_type = 'payout.qr_cleanup.v1' AND aggregate_id = CAST(:id AS uuid)"""), {"id": payout_two["id"]})
            for _ in range(30):
                async with engine.connect() as connection:
                    deleted = await connection.scalar(text("SELECT qr_deleted_at IS NOT NULL FROM payout_requests WHERE id = CAST(:id AS uuid)"), {"id": payout_two["id"]})
                if deleted:
                    break
                await asyncio.sleep(1)
            assert deleted
        finally:
            await engine.dispose()
