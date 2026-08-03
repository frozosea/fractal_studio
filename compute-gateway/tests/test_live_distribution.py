"""Opt-in proof that a running Gateway distributes real Compute work."""

from __future__ import annotations

import asyncio
import os
import time
import uuid

import asyncpg
import httpx
import pytest

LIVE_URL = os.getenv("GATEWAY_LIVE_URL")
LIVE_KEY = os.getenv("GATEWAY_LIVE_SERVICE_KEY")
LIVE_DATABASE_URL = os.getenv("GATEWAY_LIVE_DATABASE_URL")
pytestmark = pytest.mark.skipif(
    not (LIVE_URL and LIVE_KEY and LIVE_DATABASE_URL),
    reason="set GATEWAY_LIVE_URL, GATEWAY_LIVE_SERVICE_KEY and GATEWAY_LIVE_DATABASE_URL",
)


def _request(label: str) -> dict[str, object]:
    return {
        "schemaVersion": 1,
        "kind": "map_image",
        "idempotencyKey": f"gateway-live-distribution:{label}:{uuid.uuid4()}",
        "payload": {
            "centerRe": -0.75,
            "centerIm": 0.0,
            "scale": 3.0,
            "width": 64,
            "height": 64,
            "iterations": 128,
            "variant": "mandelbrot",
            "engine": "openmp",
            "scalarType": "fp64",
        },
    }


@pytest.mark.asyncio
async def test_two_real_compute_nodes_share_durable_work() -> None:
    assert LIVE_URL and LIVE_KEY and LIVE_DATABASE_URL
    headers = {"Authorization": f"Bearer {LIVE_KEY}"}
    requests = [_request("a"), _request("b")]
    async with httpx.AsyncClient(base_url=LIVE_URL, headers=headers, timeout=30, trust_env=False) as client:
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            capabilities = await client.get("/compute/v1/capabilities")
            if capabilities.status_code == 200 and capabilities.json()["gateway"]["healthyNodes"] == 2:
                break
            await asyncio.sleep(0.5)
        else:
            pytest.fail(f"Gateway did not expose two healthy nodes: {capabilities.text}")

        created = await asyncio.gather(*(client.post("/compute/v1/runs", json=item) for item in requests))
        assert all(item.status_code == 202 for item in created), [item.text for item in created]
        run_ids = [item.json()["data"]["computeRunId"] for item in created]

        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            statuses = await asyncio.gather(*(client.get(f"/compute/v1/runs/{run_id}") for run_id in run_ids))
            assert all(item.status_code == 200 for item in statuses), [item.text for item in statuses]
            if all(item.json()["data"]["status"] == "completed" for item in statuses):
                break
            await asyncio.sleep(0.5)
        else:
            pytest.fail("real Gateway runs did not complete")

    connection = await asyncpg.connect(LIVE_DATABASE_URL)
    try:
        rows = await connection.fetch(
            """
            select n.node_key
            from compute_runs r join compute_nodes n on n.id = r.node_id
            where r.idempotency_key = any($1::text[])
            order by r.idempotency_key
            """,
            [str(item["idempotencyKey"]) for item in requests],
        )
    finally:
        await connection.close()
    assert {row["node_key"] for row in rows} == {"compute-a", "compute-b"}
