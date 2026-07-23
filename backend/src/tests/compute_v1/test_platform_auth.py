from __future__ import annotations

from .client import ComputeClient
from .platform_payloads import platform_map, platform_preview


def test_platform_preview_requires_service_key(client: ComputeClient) -> None:
    response = client.request(
        "/api/map/render-inline", body=platform_preview(), authorized=False
    )

    assert response.status == 401
    assert set(response.json()) == {"code", "message", "requestId"}
    assert response.json()["code"] == "compute_unauthorized"


def test_platform_durable_route_rejects_wrong_service_key(client: ComputeClient) -> None:
    response = client.request(
        "/api/map/render",
        body=platform_map(),
        authorized=False,
        headers={"Authorization": "Bearer wrong-key"},
    )

    assert response.status == 401
    assert response.json()["code"] == "compute_unauthorized"


def test_authenticated_legacy_shape_keeps_legacy_response(client: ComputeClient) -> None:
    payload = platform_preview()
    payload.update({"requestId": "legacy-client-1", "engine": "openmp", "scalarType": "fp64"})

    response = client.request("/api/map/render-inline", body=payload)

    assert response.status == 200
    assert response.headers["X-FSD-Request-Id"] == "legacy-client-1"
