from __future__ import annotations

from .client import ComputeClient
from .payloads import transition_image_payload


def test_capabilities_advertise_transition_static_image(client: ComputeClient) -> None:
    capabilities = client.request("/compute/v1/capabilities").json()
    job = next(item for item in capabilities["jobs"] if item["kind"] == "transition_image")

    assert job["preview"] is True
    assert job["persistent"] is True
    assert job["orbitProgram"] is False
    assert capabilities["coloring"]["staticImageColorModes"] == ["direct", "eq_full", "eq_center"]


def test_pair_transition_image_preview_returns_rgba(client: ComputeClient) -> None:
    response = client.preview("transition_image", transition_image_payload())

    assert response.status == 200, response.content
    assert response.headers["X-FSD-Pixel-Format"] == "rgba8"
    assert len(response.content) == 64 * 64 * 4


def test_multi_transition_image_preview_returns_rgba(client: ComputeClient) -> None:
    response = client.preview("transition_image", transition_image_payload(multi=True))

    assert response.status == 200, response.content
    assert len(response.content) == 64 * 64 * 4
