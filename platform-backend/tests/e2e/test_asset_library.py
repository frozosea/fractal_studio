"""Opt-in T08 API smoke against Compose with its real M7 worker and Compute stub."""

from __future__ import annotations

import asyncio
import os
import uuid

import httpx
import pytest


@pytest.mark.skipif(
    not (
        os.getenv("E2E_API_URL")
        and os.getenv("E2E_COMPUTE_AVAILABLE") == "1"
        and os.getenv("E2E_PLATFORM_WORKER") == "1"
    ),
    reason="set E2E_API_URL, E2E_COMPUTE_AVAILABLE=1 and E2E_PLATFORM_WORKER=1",
)
@pytest.mark.asyncio
async def test_asset_library_derivatives_and_owner_download(e2e_api_url: str) -> None:
    suffix = uuid.uuid4().hex[:10]
    async with httpx.AsyncClient(base_url=e2e_api_url, timeout=30, trust_env=False) as owner:
        register = await owner.post(
            "/v1/auth/register",
            json={"email": f"asset-{suffix}@example.test", "password": "correct-horse-01"},
        )
        assert register.status_code == 201
        recipe = await owner.post(
            "/v1/recipes",
            headers={"Idempotency-Key": "recipe"},
            json={"canonicalSpec": {"version": 1, "seed": 808}},
        )
        assert recipe.status_code == 201
        job = await owner.post(
            "/v1/render-jobs",
            headers={"Idempotency-Key": "asset-image"},
            json={
                "recipeId": recipe.json()["data"]["id"],
                "output": {"kind": "image", "format": "png", "width": 64, "height": 64},
            },
        )
        assert job.status_code == 202
        job_id = job.json()["data"]["id"]
        asset_id: str | None = None
        for _ in range(30):
            current = await owner.get(f"/v1/render-jobs/{job_id}")
            assert current.status_code == 200
            asset_id = current.json()["data"].get("assetId")
            if current.json()["data"]["status"] == "completed" and asset_id:
                break
            await asyncio.sleep(1)
        assert asset_id is not None
        asset = None
        purposes: set[str] = set()
        for _ in range(15):
            asset = await owner.get(f"/v1/me/assets/{asset_id}")
            assert asset.status_code == 200
            purposes = {item["purpose"] for item in asset.json()["data"]["files"]}
            if {"master", "thumbnail", "watermarked_preview"}.issubset(purposes):
                break
            await asyncio.sleep(1)
        assert {"master", "thumbnail", "watermarked_preview"}.issubset(purposes)
        assert asset is not None
        assert "objectKey" not in asset.text and "runId" not in asset.text

        download = await owner.post(f"/v1/assets/{asset_id}/download-url")
        assert download.status_code == 200
        assert "expiresAt" in download.json()["data"]
        hidden = await owner.patch(
            f"/v1/me/assets/{asset_id}",
            headers={"Idempotency-Key": "hide"},
            json={"visibility": "hidden"},
        )
        assert hidden.status_code == 200

    async with httpx.AsyncClient(base_url=e2e_api_url, timeout=30, trust_env=False) as other:
        assert (
            await other.post(
                "/v1/auth/register",
                json={"email": f"other-{suffix}@example.test", "password": "correct-horse-01"},
            )
        ).status_code == 201
        assert (await other.post(f"/v1/assets/{asset_id}/download-url")).status_code == 403
