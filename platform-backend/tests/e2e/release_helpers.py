"""Black-box helpers for the isolated T14 release suite."""

from __future__ import annotations

import asyncio
import os
import uuid
from collections.abc import Awaitable, Callable
from typing import Any
from urllib.parse import urlencode

import httpx
import qrcode


CANONICAL_SPEC = {"version": 1, "seed": 42, "iterations": 64}


def release_api_url() -> str:
    value = os.getenv("E2E_API_URL")
    if not value:
        raise RuntimeError("set E2E_API_URL for the release E2E suite")
    return value


def alipay_stub_url() -> str:
    value = os.getenv("E2E_ALIPAY_STUB_URL")
    if not value:
        raise RuntimeError("set E2E_ALIPAY_STUB_URL for the release E2E suite")
    return value


async def register(client: httpx.AsyncClient, *, label: str) -> dict[str, Any]:
    response = await client.post(
        "/v1/auth/register",
        json={"email": f"{label}-{uuid.uuid4().hex[:12]}@e2e.invalid", "password": "e2e-password-01"},
    )
    assert response.status_code == 201, response.text
    return response.json()["data"]


async def login(client: httpx.AsyncClient, *, email: str, password: str) -> dict[str, Any]:
    response = await client.post("/v1/auth/login", json={"email": email, "password": password})
    assert response.status_code == 200, response.text
    return response.json()["data"]


async def become_creator(client: httpx.AsyncClient, *, label: str) -> dict[str, Any]:
    response = await client.patch(
        "/v1/me/creator-profile",
        headers={"Idempotency-Key": f"creator-profile-{label}"},
        json={"handle": f"{label}{uuid.uuid4().hex[:8]}", "displayName": f"E2E {label}"},
    )
    assert response.status_code == 200, response.text
    return response.json()["data"]


async def create_ready_asset(client: httpx.AsyncClient, *, label: str) -> str:
    recipe = await client.post(
        "/v1/recipes",
        headers={"Idempotency-Key": f"recipe-{label}"},
        json={"canonicalSpec": CANONICAL_SPEC},
    )
    assert recipe.status_code in {200, 201}, recipe.text
    recipe_id = recipe.json()["data"]["id"]
    created = await client.post(
        "/v1/render-jobs",
        headers={"Idempotency-Key": f"render-{label}"},
        json={"recipeId": recipe_id, "output": {"kind": "image", "format": "png", "width": 64, "height": 64}},
    )
    assert created.status_code == 202, created.text
    job_id = created.json()["data"]["id"]
    job = await wait_for(
        lambda: get_json(client, f"/v1/render-jobs/{job_id}"),
        lambda value: value["data"]["status"] in {"completed", "failed", "cancelled"},
    )
    assert job["data"]["status"] == "completed", job
    asset_id = str(job["data"]["assetId"])
    asset = await wait_for(
        lambda: get_json(client, f"/v1/me/assets/{asset_id}"),
        lambda value: value["data"]["derivativeStatus"] in {"ready", "failed"},
    )
    assert asset["data"]["derivativeStatus"] == "ready", asset
    return asset_id


async def create_published_listing(client: httpx.AsyncClient, *, label: str, price: str = "19.90") -> dict[str, Any]:
    asset_id = await create_ready_asset(client, label=label)
    created = await client.post(
        "/v1/listings",
        headers={"Idempotency-Key": f"listing-{label}"},
        json={
            "assetId": asset_id,
            "title": f"E2E {label}",
            "description": "release suite asset",
            "tags": ["e2e"],
            "price": price,
            "licenceOffer": {"code": "personal", "termsVersion": "v1"},
        },
    )
    assert created.status_code == 201, created.text
    listing = created.json()["data"]
    published = await client.post(
        f"/v1/listings/{listing['id']}/publish", headers={"Idempotency-Key": f"publish-{label}"}
    )
    assert published.status_code == 200, published.text
    return published.json()["data"]


async def settle_checkout(
    *, buyer: httpx.AsyncClient, listing: dict[str, Any], trade_status: str = "TRADE_SUCCESS"
) -> dict[str, Any]:
    checkout = await buyer.post(
        "/v1/checkout",
        headers={"Idempotency-Key": f"checkout-{uuid.uuid4().hex}"},
        json={
            "listingId": listing["id"],
            "licenceOfferId": listing["licenceOffer"]["id"],
            "channel": "desktop_web",
        },
    )
    assert checkout.status_code == 201, checkout.text
    payment = checkout.json()["data"]
    async with httpx.AsyncClient(base_url=alipay_stub_url(), timeout=15, trust_env=False) as stub:
        notice = await stub.post(
            "/test/notifications",
            json={
                "outTradeNo": payment["paymentAttempt"]["outTradeNo"],
                "tradeStatus": trade_status,
                "totalAmount": payment["order"]["amount"],
            },
        )
        assert notice.status_code == 200, notice.text
    webhook = await buyer.post(
        "/v1/webhooks/alipay",
        content=urlencode(notice.json()),
        headers={"Content-Type": "application/x-www-form-urlencoded"},
    )
    assert webhook.status_code == 200 and webhook.text == "success", webhook.text
    return payment


async def get_json(client: httpx.AsyncClient, path: str) -> dict[str, Any]:
    response = await client.get(path)
    assert response.status_code == 200, response.text
    return response.json()


async def wait_for(
    read: Callable[[], Awaitable[dict[str, Any]]], predicate: Callable[[dict[str, Any]], bool], timeout: float = 40
) -> dict[str, Any]:
    deadline = asyncio.get_running_loop().time() + timeout
    while asyncio.get_running_loop().time() < deadline:
        value = await read()
        if predicate(value):
            return value
        await asyncio.sleep(0.4)
    raise AssertionError("timed out waiting for an externally observable state transition")


def qr_png(label: str) -> bytes:
    image = qrcode.make(f"https://payout.e2e.invalid/{label}")
    from io import BytesIO

    output = BytesIO()
    image.save(output, format="PNG")
    return output.getvalue()


def assert_error(response: httpx.Response, *, status: int, code: str) -> None:
    assert response.status_code == status, response.text
    body = response.json()
    assert body == {"error": {"code": code, "message": body["error"]["message"], "details": {}}}
