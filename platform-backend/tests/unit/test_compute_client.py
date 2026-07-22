from __future__ import annotations

from uuid import UUID

import httpx
import pytest

from app.core.config import Settings
from app.infrastructure.compute.compute_client import ComputeClient, ComputeError


def settings() -> Settings:
    return Settings(
        environment="test",
        database_url="postgresql+asyncpg://unused/unused",
        redis_url="redis://unused",
        compute_base_url="http://compute.test",
        compute_service_key="secret",
        compute_node_id="test-node",
        foundation_routes_enabled=True,
        foundation_subject_id=UUID("00000000-0000-0000-0000-000000000001"),
        outbox_poll_seconds=0.01,
        outbox_lease_seconds=30,
        outbox_batch_size=10,
    )


@pytest.mark.asyncio
async def test_create_run_uses_private_contract_and_idempotency_key() -> None:
    async def handler(request: httpx.Request) -> httpx.Response:
        assert request.headers["Authorization"] == "Bearer secret"
        body = __import__("json").loads(request.content)
        assert body["schemaVersion"] == 1
        assert body["idempotencyKey"] == "platform-job"
        return httpx.Response(202, json={"schemaVersion": 1, "data": {
            "computeRunId": "compute-run", "status": "queued", "kind": "map_image"
        }})

    transport = httpx.MockTransport(handler)
    async with httpx.AsyncClient(transport=transport) as http:
        client = ComputeClient(settings(), http)
        result = await client.create_run("map_image", {"width": 64}, "platform-job", "request-id")
    assert result["computeRunId"] == "compute-run"


@pytest.mark.asyncio
async def test_compute_error_preserves_structured_error() -> None:
    def handler(_: httpx.Request) -> httpx.Response:
        return httpx.Response(422, json={"error": {
            "code": "UNSUPPORTED_CAPABILITY", "message": "unsupported", "details": {"kind": "x"}
        }})

    async with httpx.AsyncClient(transport=httpx.MockTransport(handler)) as http:
        client = ComputeClient(settings(), http)
        with pytest.raises(ComputeError) as captured:
            await client.capabilities()
    assert captured.value.code == "UNSUPPORTED_CAPABILITY"
    assert captured.value.status_code == 422

