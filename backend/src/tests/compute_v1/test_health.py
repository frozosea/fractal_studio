from __future__ import annotations

from .client import ComputeClient


def test_health_is_public(client: ComputeClient) -> None:
    result = client.request("/compute/v1/health", authorized=False)

    assert result.status == 200
    assert result.json()["status"] == "ok"
