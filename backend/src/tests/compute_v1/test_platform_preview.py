from __future__ import annotations

from .client import ComputeClient
from .platform_payloads import platform_preview


def test_platform_preview_returns_exact_rgba_frame(client: ComputeClient) -> None:
    payload = platform_preview()

    response = client.request("/api/map/render-inline", body=payload)

    assert response.status == 200
    assert response.headers["Content-Type"] == "application/octet-stream"
    assert response.headers["X-FSD-Width"] == str(payload["width"])
    assert response.headers["X-FSD-Height"] == str(payload["height"])
    assert response.headers["X-FSD-Pixel-Format"].lower() == "rgba8"
    assert len(response.content) == payload["width"] * payload["height"] * 4


def test_platform_preview_limit_returns_flat_problem(client: ComputeClient) -> None:
    payload = platform_preview()
    payload["width"] = 1025

    response = client.request("/api/map/render-inline", body=payload)

    assert response.status == 413
    assert response.json()["code"] == "request_limit_exceeded"
    assert set(response.json()) == {"code", "message", "requestId"}
