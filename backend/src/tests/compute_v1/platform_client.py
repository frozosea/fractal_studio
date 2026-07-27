from __future__ import annotations

import time
from typing import Any

from .client import ComputeClient, HttpResult


def create_platform_run(
    client: ComputeClient, route: str, payload: dict[str, Any]
) -> tuple[str, HttpResult]:
    response = client.request(route, body=payload)
    assert response.status == 200, response.content
    data = response.json()
    assert data["clientJobId"] == payload["clientJobId"]
    return str(data["runId"]), response


def platform_status(client: ComputeClient, run_id: str) -> HttpResult:
    return client.request(f"/api/runs/status?runId={run_id}")


def wait_for_platform_run(
    client: ComputeClient, run_id: str, *, timeout: float = 15.0
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        response = platform_status(client, run_id)
        assert response.status == 200, response.content
        data = response.json()
        if data["status"] in {"completed", "failed", "cancelled"}:
            return data
        time.sleep(0.02)
    raise AssertionError(f"platform run {run_id} did not reach a terminal state")
