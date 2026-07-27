from __future__ import annotations

import uuid

from .client import ComputeClient
from .platform_client import create_platform_run, platform_status, wait_for_platform_run
from .platform_payloads import (
    platform_hs_mesh,
    platform_map,
    platform_transition_mesh,
    platform_video,
)


def test_platform_map_run_is_idempotent_and_has_png(client: ComputeClient) -> None:
    payload = platform_map()
    first_id, first = create_platform_run(client, "/api/map/render", payload)
    second_id, second = create_platform_run(client, "/api/map/render", payload)

    assert first.json()["status"] in {"queued", "running", "completed"}
    assert second_id == first_id
    terminal = wait_for_platform_run(client, first_id)
    assert terminal["status"] == "completed"
    assert terminal["progressPercent"] == 100
    assert {item["mediaType"] for item in terminal["artifacts"]} >= {"image/png"}


def test_platform_client_job_id_conflict_is_flat_problem(client: ComputeClient) -> None:
    payload = platform_map()
    create_platform_run(client, "/api/map/render", payload)
    payload["iterations"] += 1

    conflict = client.request("/api/map/render", body=payload)

    assert conflict.status == 409
    assert conflict.json()["code"] == "idempotency_conflict"
    assert set(conflict.json()) == {"code", "message", "requestId"}


def test_platform_status_hides_non_platform_run(client: ComputeClient) -> None:
    legacy_id, _ = client.create_run("map_image", {
        "centerRe": -0.75, "centerIm": 0.0, "scale": 3.0,
        "width": 64, "height": 64, "iterations": 8,
        "variant": "mandelbrot", "engine": "openmp", "scalarType": "fp64",
    })

    response = platform_status(client, legacy_id)

    assert response.status == 200
    assert "clientJobId" not in response.json()


def test_unknown_platform_run_returns_flat_not_found(client: ComputeClient) -> None:
    response = platform_status(client, f"missing-{uuid.uuid4()}")

    assert response.status == 404
    assert response.json()["code"] == "run_not_found"
    assert set(response.json()) == {"code", "message", "requestId"}


def test_platform_cancel_returns_contract_response(client: ComputeClient) -> None:
    payload = platform_map()
    payload.update({"width": 512, "height": 512, "iterations": 100_000})
    run_id, _ = create_platform_run(client, "/api/map/render", payload)

    response = client.request("/api/runs/cancel", body={"runId": run_id})

    assert response.status == 200
    assert response.json() == {"runId": run_id, "status": "cancel_requested"}
    assert wait_for_platform_run(client, run_id)["status"] in {"cancelled", "completed"}


def test_all_platform_durable_routes_return_flat_acceptance(client: ComputeClient) -> None:
    requests = (
        ("/api/video/export", platform_video()),
        ("/api/hs/mesh", platform_hs_mesh()),
        ("/api/transition/mesh", platform_transition_mesh()),
    )

    for route, payload in requests:
        run_id, response = create_platform_run(client, route, payload)
        assert response.json()["runId"] == run_id
        assert set(response.json()) == {"runId", "clientJobId", "status"}
        wait_for_platform_run(client, run_id, timeout=30.0)
