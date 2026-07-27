"""Opt-in M4 E2E against Compose API, worker, MinIO and Compute service."""

from __future__ import annotations

import asyncio
import os
import uuid

import httpx
import pytest
from sqlalchemy import text
from sqlalchemy.ext.asyncio import create_async_engine


pytestmark = pytest.mark.skipif(
    not (
        os.getenv("E2E_API_URL")
        and os.getenv("E2E_COMPUTE_AVAILABLE") == "1"
        and os.getenv("E2E_PLATFORM_WORKER") == "1"
        and os.getenv("E2E_DATABASE_URL")
    ),
    reason="set E2E_API_URL, E2E_DATABASE_URL, E2E_COMPUTE_AVAILABLE=1 and E2E_PLATFORM_WORKER=1",
)


async def _prepare_unpublishable_asset(asset_id: str, *, mode: str) -> None:
    """Prepare lifecycle state unavailable from browser API; publish itself stays a real API call."""
    engine = create_async_engine(os.environ["E2E_DATABASE_URL"])
    try:
        async with engine.begin() as connection:
            if mode == "missing_derivatives":
                await connection.execute(
                    text(
                        "DELETE FROM asset_files WHERE asset_id = CAST(:asset_id AS uuid) "
                        "AND purpose IN ('thumbnail', 'watermarked_preview', 'video_poster')"
                    ),
                    {"asset_id": asset_id},
                )
            elif mode == "unready":
                await connection.execute(
                    text("UPDATE assets SET status = 'processing' WHERE id = CAST(:asset_id AS uuid)"),
                    {"asset_id": asset_id},
                )
            else:
                raise AssertionError(f"unknown asset setup mode: {mode}")
    finally:
        await engine.dispose()


async def _ready_image(owner: httpx.AsyncClient, *, seed: int, key: str) -> str:
    recipe = await owner.post(
        "/v1/recipes", headers={"Idempotency-Key": f"{key}-recipe"}, json={"canonicalSpec": {"version": 1, "seed": seed}}
    )
    assert recipe.status_code == 201, recipe.text
    job = await owner.post(
        "/v1/render-jobs",
        headers={"Idempotency-Key": f"{key}-render"},
        json={
            "recipeId": recipe.json()["data"]["id"],
            "output": {"kind": "image", "format": "png", "width": 64, "height": 64},
        },
    )
    assert job.status_code == 202, job.text
    job_id = job.json()["data"]["id"]
    asset_id: str | None = None
    for _ in range(45):
        current = await owner.get(f"/v1/render-jobs/{job_id}")
        assert current.status_code == 200, current.text
        asset_id = current.json()["data"].get("assetId")
        if current.json()["data"]["status"] == "completed" and asset_id:
            break
        await asyncio.sleep(1)
    assert asset_id is not None
    for _ in range(30):
        asset = await owner.get(f"/v1/me/assets/{asset_id}")
        assert asset.status_code == 200, asset.text
        purposes = {item["purpose"] for item in asset.json()["data"]["files"]}
        if {"thumbnail", "watermarked_preview"}.issubset(purposes):
            return asset_id
        await asyncio.sleep(1)
    raise AssertionError("image listing derivatives were not created")


async def _become_creator(client: httpx.AsyncClient, *, suffix: str, key: str) -> None:
    profile = await client.patch(
        "/v1/me/creator-profile",
        headers={"Idempotency-Key": key},
        json={"handle": f"m4_{suffix}", "displayName": f"M4 {suffix}"},
    )
    assert profile.status_code == 200, profile.text
    assert "creator" in profile.json()["data"]["roles"]


@pytest.mark.asyncio
async def test_catalogue_listing_versions_and_favourites(e2e_api_url: str) -> None:
    suffix = uuid.uuid4().hex[:10]
    async with httpx.AsyncClient(base_url=e2e_api_url, timeout=45, trust_env=False) as owner, httpx.AsyncClient(
        base_url=e2e_api_url, timeout=45, trust_env=False
    ) as buyer, httpx.AsyncClient(base_url=e2e_api_url, timeout=45, trust_env=False) as guest:
        registered = await owner.post(
            "/v1/auth/register", json={"email": f"market-owner-{suffix}@example.test", "password": "correct-horse-01"}
        )
        assert registered.status_code == 201, registered.text
        await _become_creator(owner, suffix=f"owner_{suffix}", key="owner-profile")

        first_asset = await _ready_image(owner, seed=1001, key="first")
        payload = {
            "assetId": first_asset,
            "title": "Orbit",
            "description": "Test fractal catalogue entry",
            "tags": ["Orbit", "fractal"],
            "price": "19.90",
            "licenceOffer": {"code": "personal", "termsVersion": "v1"},
        }
        draft = await owner.post("/v1/listings", headers={"Idempotency-Key": "listing-one"}, json=payload)
        assert draft.status_code == 201, draft.text
        listing_id = draft.json()["data"]["id"]
        assert draft.json()["data"]["status"] == "draft"
        assert draft.json()["data"]["tags"] == ["fractal", "orbit"]
        replay = await owner.post("/v1/listings", headers={"Idempotency-Key": "listing-one"}, json=payload)
        assert replay.status_code == 201 and replay.json()["data"]["id"] == listing_id
        assert (await guest.get(f"/v1/listings/{listing_id}")).status_code == 404
        assert (await owner.patch(f"/v1/listings/{listing_id}", headers={"Idempotency-Key": "price-bad"}, json={"price": "19.999"})).status_code == 422

        published = await owner.post(f"/v1/listings/{listing_id}/publish", headers={"Idempotency-Key": "publish-one"})
        assert published.status_code == 200, published.text
        assert published.json()["data"]["status"] == "published"
        assert published.json()["data"]["preview"]["watermarkedPreviewUrl"]
        assert (await owner.patch(f"/v1/listings/{listing_id}", headers={"Idempotency-Key": "published-edit"}, json={"title": "blocked"})).status_code == 409

        public = await guest.get(f"/v1/listings/{listing_id}")
        assert public.status_code == 200, public.text
        assert "objectKey" not in public.text and "master" not in public.text and "originalUrl" not in public.text

        second_asset = await _ready_image(owner, seed=1002, key="second")
        second = await owner.post(
            "/v1/listings",
            headers={"Idempotency-Key": "listing-two"},
            json={
                "assetId": second_asset, "title": "Nebula", "description": "Second fractal", "tags": ["nebula"],
                "price": "29.90", "licenceOffer": {"code": "commercial", "termsVersion": "v1"},
            },
        )
        assert second.status_code == 201, second.text
        second_listing = second.json()["data"]["id"]
        assert (await owner.post(f"/v1/listings/{second_listing}/publish", headers={"Idempotency-Key": "publish-two"})).status_code == 200

        first_page = await guest.get("/v1/explore", params={"limit": 1})
        assert first_page.status_code == 200, first_page.text
        cursor = first_page.json()["page"]["nextCursor"]
        assert cursor
        second_page = await guest.get("/v1/explore", params={"limit": 1, "cursor": cursor})
        assert second_page.status_code == 200 and len(second_page.json()["data"]) == 1, second_page.text
        mismatch = await guest.get("/v1/explore", params={"limit": 1, "tag": "orbit", "cursor": cursor})
        assert mismatch.status_code == 422
        searched = await guest.get(
            "/v1/explore",
            params={
                "q": "Orbit",
                "tag": "orbit",
                "creator": draft.json()["data"]["creator"]["handle"],
                "sort": "relevance",
            },
        )
        assert searched.status_code == 200 and [item["id"] for item in searched.json()["data"]] == [listing_id], searched.text

        buyer_registered = await buyer.post(
            "/v1/auth/register", json={"email": f"market-buyer-{suffix}@example.test", "password": "correct-horse-01"}
        )
        assert buyer_registered.status_code == 201, buyer_registered.text
        creator_page = await owner.get("/v1/me/listings", params={"status": "published", "limit": 1})
        assert creator_page.status_code == 200 and creator_page.json()["page"]["nextCursor"], creator_page.text
        assert (
            await owner.get(
                "/v1/me/listings",
                params={"status": "published", "limit": 1, "cursor": creator_page.json()["page"]["nextCursor"]},
            )
        ).status_code == 200
        favorite = await buyer.post(f"/v1/assets/{first_asset}/favorite", headers={"Idempotency-Key": "favorite-one"})
        assert favorite.status_code == 201, favorite.text
        assert favorite.json()["data"]["listing"]["id"] == listing_id
        assert (await buyer.post(f"/v1/assets/{first_asset}/favorite", headers={"Idempotency-Key": "favorite-one"})).status_code == 201
        assert (await buyer.post(f"/v1/assets/{second_asset}/favorite", headers={"Idempotency-Key": "favorite-two"})).status_code == 201
        favorites_page = await buyer.get("/v1/me/favorites", params={"limit": 1})
        assert favorites_page.status_code == 200 and favorites_page.json()["page"]["nextCursor"], favorites_page.text
        assert (
            await buyer.get("/v1/me/favorites", params={"limit": 1, "cursor": favorites_page.json()["page"]["nextCursor"]})
        ).status_code == 200
        assert (await buyer.delete(f"/v1/assets/{first_asset}/favorite", headers={"Idempotency-Key": "favorite-delete"})).status_code == 204
        assert (await buyer.delete(f"/v1/assets/{first_asset}/favorite", headers={"Idempotency-Key": "favorite-delete"})).status_code == 204
        assert (await buyer.delete(f"/v1/assets/{second_asset}/favorite", headers={"Idempotency-Key": "favorite-delete-two"})).status_code == 204
        assert (await buyer.get("/v1/me/favorites")).json()["data"] == []

        unpublished = await owner.post(f"/v1/listings/{listing_id}/unpublish", headers={"Idempotency-Key": "unpublish-one"})
        assert unpublished.status_code == 200 and unpublished.json()["data"]["status"] == "unpublished", unpublished.text
        assert (await guest.get(f"/v1/listings/{listing_id}")).status_code == 404
        redraft = await owner.patch(
            f"/v1/listings/{listing_id}", headers={"Idempotency-Key": "redraft-one"}, json={"title": "Orbit revised"}
        )
        assert redraft.status_code == 200 and redraft.json()["data"]["status"] == "draft", redraft.text
        republished = await owner.post(f"/v1/listings/{listing_id}/publish", headers={"Idempotency-Key": "republish-one"})
        assert republished.status_code == 200 and republished.json()["data"]["title"] == "Orbit revised", republished.text
        mine = await owner.get("/v1/me/listings", params={"status": "published"})
        assert mine.status_code == 200 and {row["id"] for row in mine.json()["data"]} == {listing_id, second_listing}

        hidden_asset = await _ready_image(owner, seed=1003, key="hidden")
        hidden = await owner.patch(
            f"/v1/me/assets/{hidden_asset}", headers={"Idempotency-Key": "hide-for-listing"}, json={"visibility": "hidden"}
        )
        assert hidden.status_code == 200, hidden.text
        hidden_draft = await owner.post(
            "/v1/listings",
            headers={"Idempotency-Key": "hidden-listing"},
            json={
                "assetId": hidden_asset, "title": "Hidden", "description": "", "tags": [], "price": "10.00",
                "licenceOffer": {"code": "personal", "termsVersion": "v1"},
            },
        )
        assert hidden_draft.status_code == 201, hidden_draft.text
        assert (
            await owner.post(
                f"/v1/listings/{hidden_draft.json()['data']['id']}/publish",
                headers={"Idempotency-Key": "publish-hidden"},
            )
        ).status_code == 409

        for mode, key in (("missing_derivatives", "missing"), ("unready", "unready")):
            unpublishable_asset = await _ready_image(owner, seed=1004 if key == "missing" else 1005, key=key)
            await _prepare_unpublishable_asset(unpublishable_asset, mode=mode)
            invalid_draft = await owner.post(
                "/v1/listings",
                headers={"Idempotency-Key": f"{key}-listing"},
                json={
                    "assetId": unpublishable_asset,
                    "title": key,
                    "description": "",
                    "tags": [],
                    "price": "10.00",
                    "licenceOffer": {"code": "personal", "termsVersion": "v1"},
                },
            )
            assert invalid_draft.status_code == 201, invalid_draft.text
            assert (
                await owner.post(
                    f"/v1/listings/{invalid_draft.json()['data']['id']}/publish",
                    headers={"Idempotency-Key": f"publish-{key}"},
                )
            ).status_code == 409

        await _become_creator(buyer, suffix=f"buyer_{suffix}", key="buyer-profile")
        foreign = await buyer.post("/v1/listings", headers={"Idempotency-Key": "foreign"}, json=payload)
        assert foreign.status_code == 404
        assert (
            await buyer.post(f"/v1/listings/{listing_id}/publish", headers={"Idempotency-Key": "foreign-publish"})
        ).status_code == 404
