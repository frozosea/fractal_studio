"""T15 black-box proof against a real C++ Compute v1 process, never the Python stub."""

from __future__ import annotations

import os
import time
import uuid

import httpx
import pytest


pytestmark = pytest.mark.skipif(
    not (os.getenv("E2E_REAL_COMPUTE_URL") and os.getenv("E2E_REAL_COMPUTE_SERVICE_KEY")),
    reason="set E2E_REAL_COMPUTE_URL and E2E_REAL_COMPUTE_SERVICE_KEY",
)


def _payload(*, iterations: int = 32) -> dict[str, object]:
    return {
        "schemaVersion": 1,
        "kind": "map_image",
        "idempotencyKey": f"platform-contract:{uuid.uuid4()}",
        "payload": {
            "centerRe": -0.75,
            "centerIm": 0.0,
            "scale": 3.0,
            "width": 64,
            "height": 64,
            "iterations": iterations,
            "variant": "mandelbrot",
            "engine": "openmp",
            "scalarType": "fp64",
        },
    }


def test_real_compute_v1_private_idempotent_and_legacy_off() -> None:
    base_url = os.environ["E2E_REAL_COMPUTE_URL"]
    key = os.environ["E2E_REAL_COMPUTE_SERVICE_KEY"]
    with httpx.Client(base_url=base_url, timeout=30, trust_env=False) as client:
        health = client.get("/compute/v1/health")
        assert health.status_code == 200 and health.json()["status"] == "ok"
        unauthenticated = client.get("/compute/v1/capabilities")
        assert unauthenticated.status_code == 401
        assert unauthenticated.json()["error"]["code"] == "COMPUTE_UNAUTHORIZED"
        legacy = client.get("/api/system/check")
        assert legacy.status_code == 404

        headers = {"Authorization": f"Bearer {key}"}
        request = _payload()
        first = client.post("/compute/v1/runs", headers=headers, json=request)
        replay = client.post("/compute/v1/runs", headers=headers, json=request)
        assert first.status_code == replay.status_code == 202
        run_id = first.json()["data"]["computeRunId"]
        assert replay.json()["data"]["computeRunId"] == run_id

        conflict = {**request, "payload": {**request["payload"], "iterations": 33}}
        rejected = client.post("/compute/v1/runs", headers=headers, json=conflict)
        assert rejected.status_code == 409
        assert rejected.json()["error"]["code"] == "IDEMPOTENCY_CONFLICT"

        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            state = client.get(f"/compute/v1/runs/{run_id}", headers=headers)
            assert state.status_code == 200, state.text
            if state.json()["data"]["status"] == "completed":
                break
            time.sleep(0.2)
        else:
            raise AssertionError("real Compute run did not complete")
        manifest = client.get(f"/compute/v1/runs/{run_id}/manifest", headers=headers)
        assert manifest.status_code == 200, manifest.text
        artifact = next(item for item in manifest.json()["artifacts"] if item["mediaType"] == "image/png")
        assert artifact["artifactId"] and len(artifact["sha256"]) == 64 and artifact["sizeBytes"] > 0
        content = client.get("/compute/v1/artifacts", headers=headers, params={"artifactId": artifact["artifactId"]})
        assert content.status_code == 200 and content.content
