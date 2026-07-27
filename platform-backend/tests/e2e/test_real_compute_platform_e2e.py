"""T15: Platform browser API uses real C++ Compute and ingests its private artifact."""

from __future__ import annotations

import os

import httpx
import pytest

from tests.e2e.release_helpers import CANONICAL_SPEC, become_creator, get_json, register, release_api_url, wait_for


pytestmark = pytest.mark.skipif(
    not os.getenv("E2E_REAL_COMPUTE_PLATFORM"),
    reason="set E2E_REAL_COMPUTE_PLATFORM for the real Compute Platform gate",
)


@pytest.mark.asyncio
async def _real_asset(client: httpx.AsyncClient, *, label: str, output: dict[str, object]) -> dict[str, object]:
    recipe = await client.post(
        "/v1/recipes", headers={"Idempotency-Key": f"real-{label}-recipe"}, json={"canonicalSpec": CANONICAL_SPEC}
    )
    assert recipe.status_code in {200, 201}, recipe.text
    created = await client.post(
        "/v1/render-jobs", headers={"Idempotency-Key": f"real-{label}-render"},
        json={"recipeId": recipe.json()["data"]["id"], "output": output},
    )
    assert created.status_code == 202, created.text
    job_id = created.json()["data"]["id"]
    terminal = await wait_for(
        lambda: get_json(client, f"/v1/render-jobs/{job_id}"),
        lambda value: value["data"]["status"] in {"completed", "failed", "cancelled"}, timeout=180,
    )
    assert terminal["data"]["status"] == "completed", terminal
    asset = await client.get(f"/v1/me/assets/{terminal['data']['assetId']}")
    assert asset.status_code == 200, asset.text
    return asset.json()["data"]


@pytest.mark.asyncio
async def test_platform_real_compute_preview_and_all_mvp_outputs_without_leakage() -> None:
    async with httpx.AsyncClient(base_url=release_api_url(), timeout=90, trust_env=False) as client:
        await register(client, label="real-compute")
        await become_creator(client, label="realcompute")
        preview = await client.post("/v1/studio/preview", json={"canonicalSpec": CANONICAL_SPEC, "width": 64, "height": 64})
        assert preview.status_code == 200 and preview.headers["content-type"].startswith("image/png"), preview.text

        image = await _real_asset(client, label="image", output={"kind": "image", "format": "png", "width": 64, "height": 64})
        video = await _real_asset(client, label="video", output={"kind": "video", "format": "mp4", "width": 128, "height": 128, "durationSeconds": 1.0, "fps": 2})
        hs_mesh = await _real_asset(client, label="hs", output={"kind": "hs_mesh", "format": "glb", "resolution": 8, "meshSpec": {}})
        transition_mesh = await _real_asset(client, label="transition", output={"kind": "transition_mesh", "format": "glb", "resolution": 8, "iterations": 16, "meshSpec": {"centerX": 0, "centerY": 0, "centerZ": 0, "extent": 2, "transitionFrom": "mandelbrot", "transitionTo": "burning_ship"}})

        assert [item["mediaType"] for item in (image, video, hs_mesh, transition_mesh)] == ["image", "video", "mesh", "mesh"]
        for payload in (image, video, hs_mesh, transition_mesh):
            text = str(payload)
            assert payload["status"] == "ready"
            assert all(value not in text for value in ("objectKey", "computeRunId", "artifactId", "sha256"))
