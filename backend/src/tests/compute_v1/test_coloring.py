from __future__ import annotations

from .client import ComputeClient
from .platform_client import create_platform_run, wait_for_platform_run
from .platform_payloads import (
    custom_color_program,
    platform_map,
    platform_preview,
    platform_video,
)


def with_custom_color(payload: dict[str, object]) -> dict[str, object]:
    result = dict(payload)
    result.pop("colorMap", None)
    result["colorProgram"] = custom_color_program()
    return result


def test_capabilities_advertise_bounded_custom_gradient(client: ComputeClient) -> None:
    coloring = client.request("/compute/v1/capabilities").json()["coloring"]

    assert coloring["customGradient"] is True
    assert coloring["customGradientKinds"] == ["map_image"]
    assert coloring["customGradientMaxStops"] == 16


def test_custom_gradient_controls_preview_interior_pixel(client: ComputeClient) -> None:
    payload = with_custom_color(platform_preview())

    response = client.request("/api/map/render-inline", body=payload)

    assert response.status == 200, response.content
    width = int(payload["width"])
    height = int(payload["height"])
    center = ((height // 2) * width + width // 2) * 4
    assert response.content[center:center + 4] == bytes((1, 2, 3, 255))


def test_custom_gradient_is_preserved_by_png_run(client: ComputeClient) -> None:
    payload = with_custom_color(platform_map())

    run_id, _ = create_platform_run(client, "/api/map/render", payload)
    terminal = wait_for_platform_run(client, run_id)

    assert terminal["status"] == "completed"
    assert {item["mediaType"] for item in terminal["artifacts"]} >= {"image/png"}


def test_color_map_and_program_are_mutually_exclusive(client: ComputeClient) -> None:
    payload = platform_preview()
    payload["colorProgram"] = custom_color_program()

    response = client.request("/api/map/render-inline", body=payload)

    assert response.status == 400
    assert response.json()["code"] == "invalid_request"


def test_invalid_custom_gradient_returns_flat_problem(client: ComputeClient) -> None:
    payload = with_custom_color(platform_preview())
    payload["colorProgram"]["stops"][1]["color"] = "red"

    response = client.request("/api/map/render-inline", body=payload)

    assert response.status == 400
    assert response.json()["code"] == "invalid_request"
    assert set(response.json()) == {"code", "message", "requestId"}


def test_unknown_builtin_color_map_is_not_silently_replaced(client: ComputeClient) -> None:
    payload = platform_preview()
    payload["colorMap"] = "does_not_exist"

    response = client.request("/api/map/render-inline", body=payload)

    assert response.status == 400
    assert response.json()["code"] == "invalid_request"


def test_video_explicitly_rejects_custom_color_program(client: ComputeClient) -> None:
    payload = with_custom_color(platform_video())

    response = client.request("/api/video/export", body=payload)

    assert response.status == 400
    assert response.json()["code"] == "unsupported_capability"
