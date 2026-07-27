"""T14 failure/recovery cases through HTTP and externally visible test doubles only."""

from __future__ import annotations

import os
import uuid

import httpx
import pytest

from tests.e2e.release_helpers import (
    CANONICAL_SPEC,
    assert_error,
    become_creator,
    register,
    release_api_url,
    wait_for,
)


pytestmark = pytest.mark.skipif(not os.getenv("E2E_API_URL"), reason="set E2E_API_URL")


@pytest.mark.asyncio
async def test_idempotency_cancel_and_compute_retry_recover() -> None:
    async with httpx.AsyncClient(base_url=release_api_url(), timeout=60, trust_env=False) as client:
        await register(client, label="recovery")
        await become_creator(client, label="recovery")
        profile_key = "recovery-profile-conflict"
        first = await client.patch(
            "/v1/me/creator-profile",
            headers={"Idempotency-Key": profile_key},
            json={"handle": f"retry{uuid.uuid4().hex[:8]}", "displayName": "Retry One"},
        )
        assert first.status_code == 200
        conflict = await client.patch(
            "/v1/me/creator-profile",
            headers={"Idempotency-Key": profile_key},
            json={"handle": f"retry{uuid.uuid4().hex[:8]}", "displayName": "Retry Two"},
        )
        assert_error(conflict, status=409, code="idempotency_conflict")

        transient = {**CANONICAL_SPEC, "variant": "transient_failure"}
        recipe = await client.post(
            "/v1/recipes", headers={"Idempotency-Key": "recovery-recipe"}, json={"canonicalSpec": transient}
        )
        assert recipe.status_code == 201, recipe.text
        created = await client.post(
            "/v1/render-jobs",
            headers={"Idempotency-Key": "recovery-render"},
            json={
                "recipeId": recipe.json()["data"]["id"],
                "output": {"kind": "image", "format": "png", "width": 64, "height": 64},
            },
        )
        assert created.status_code == 202, created.text
        job_id = created.json()["data"]["id"]
        completed = await wait_for(
            lambda: _job(client, job_id), lambda value: value["data"]["status"] in {"completed", "failed"}
        )
        assert completed["data"]["status"] == "completed", completed

        cancelled_recipe = await client.post(
            "/v1/recipes", headers={"Idempotency-Key": "cancel-recipe"}, json={"canonicalSpec": {"version": 1, "seed": 999}}
        )
        assert cancelled_recipe.status_code == 201
        job = await client.post(
            "/v1/render-jobs",
            headers={"Idempotency-Key": "cancel-render"},
            json={
                "recipeId": cancelled_recipe.json()["data"]["id"],
                "output": {"kind": "image", "format": "png", "width": 64, "height": 64},
            },
        )
        assert job.status_code == 202
        cancelled = await client.post(
            f"/v1/render-jobs/{job.json()['data']['id']}/cancel", headers={"Idempotency-Key": "cancel-request"}
        )
        assert cancelled.status_code == 202, cancelled.text
        terminal = await wait_for(
            lambda: _job(client, job.json()["data"]["id"]),
            lambda value: value["data"]["status"] in {"cancelled", "completed", "failed"},
        )
        assert terminal["data"]["status"] == "cancelled", terminal


async def _job(client: httpx.AsyncClient, job_id: str) -> dict[str, object]:
    response = await client.get(f"/v1/render-jobs/{job_id}")
    assert response.status_code == 200, response.text
    return response.json()
