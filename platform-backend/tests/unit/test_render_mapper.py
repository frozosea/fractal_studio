"""T06 pure persisted-DTO mapper coverage."""

from __future__ import annotations

from uuid import UUID

import pytest

from app.studio.compute_request_mapper import RENDER_MAPPING_VERSION, map_durable_v1
from app.studio.models import FractalSpec
from app.studio.recipe_service import canonicalize_spec


JOB_ID = UUID("12345678-1234-5678-1234-567812345678")


def _canonical() -> dict[str, object]:
    return canonicalize_spec(FractalSpec.model_validate({"version": 1, "seed": 42})).spec


def test_durable_image_mapper_binds_job_id_before_worker_submission() -> None:
    route, body = map_durable_v1(
        _canonical(),
        output_spec={"kind": "image", "format": "png", "width": 512, "height": 256},
        client_job_id=JOB_ID,
    )

    assert RENDER_MAPPING_VERSION == "compute-v1-render-v1"
    assert route == "/compute/v1/runs"
    assert body["schemaVersion"] == 1
    assert body["kind"] == "map_image"
    assert body["idempotencyKey"] == f"platform-job:{JOB_ID}"
    assert body["payload"]["width"] == 512


def test_durable_transition_image_uses_dedicated_compute_kind() -> None:
    spec = canonicalize_spec(FractalSpec.model_validate({
        "version": 1,
        "transitionMode": "pair",
        "transitionThetaMilliDeg": -90000,
        "transitionFrom": "mandelbrot",
        "transitionTo": "burning_ship",
    })).spec

    _route, body = map_durable_v1(
        spec,
        output_spec={"kind": "image", "format": "png", "width": 512, "height": 512},
        client_job_id=JOB_ID,
    )

    assert body["kind"] == "transition_image"
    assert body["payload"]["transitionThetaMilliDeg"] == -90000
    assert body["payload"]["transitionTo"] == "burning_ship"


def test_incomplete_canonical_spec_is_a_structured_error() -> None:
    spec = _canonical()
    del spec["iterations"]

    with pytest.raises(ValueError, match="incomplete_canonical_spec"):
        map_durable_v1(
            spec,
            output_spec={"kind": "image", "format": "png", "width": 512, "height": 256},
            client_job_id=JOB_ID,
        )


def test_video_rejects_custom_gradient_per_coloring_contract() -> None:
    spec = canonicalize_spec(
        FractalSpec.model_validate(
            {
                "version": 1,
                "colorMap": None,
                "colorProgram": {
                    "stops": [{"at": 0.0, "color": "#000000"}, {"at": 1.0, "color": "#ffffff"}]
                },
            }
        )
    ).spec

    with pytest.raises(ValueError, match="color_program_unsupported_for_output"):
        map_durable_v1(
            spec,
            output_spec={
                "kind": "video",
                "format": "mp4",
                "width": 1920,
                "height": 1080,
                "durationSeconds": 30.0,
                "fps": 60,
                "depthOctaves": 12.5,
            },
            client_job_id=JOB_ID,
        )


def test_video_depth_octaves_comes_from_the_output_spec() -> None:
    _route, body = map_durable_v1(
        _canonical(),
        output_spec={
            "kind": "video",
            "format": "mp4",
            "width": 1920,
            "height": 1080,
            "durationSeconds": 30.0,
            "fps": 60,
            "depthOctaves": 12.5,
        },
        client_job_id=JOB_ID,
    )

    assert body["payload"]["depthOctaves"] == 12.5


def test_durable_video_and_mesh_mappers_allow_only_contract_routes() -> None:
    video_route, video = map_durable_v1(
        _canonical(),
        output_spec={
            "kind": "video",
            "format": "mp4",
            "width": 1920,
            "height": 1080,
            "durationSeconds": 30.0,
            "fps": 60,
        },
        client_job_id=JOB_ID,
    )
    mesh_route, mesh = map_durable_v1(
        _canonical(),
        output_spec={"kind": "hs_mesh", "format": "glb", "resolution": 128, "meshSpec": {}},
        client_job_id=JOB_ID,
    )
    transition_route, transition = map_durable_v1(
        _canonical(),
        output_spec={
            "kind": "transition_mesh",
            "format": "stl",
            "resolution": 128,
            "iterations": 100,
            "meshSpec": {
                "centerX": 0.0,
                "centerY": 0.0,
                "centerZ": 0.0,
                "extent": 2.0,
                "transitionFrom": "a",
                "transitionTo": "b",
            },
        },
        client_job_id=JOB_ID,
    )

    assert (video_route, video["kind"], video["payload"]["durationSec"], video["payload"]["fps"]) == (
        "/compute/v1/runs",
        "zoom_video",
        30.0,
        60,
    )
    assert (mesh_route, mesh["kind"], mesh["payload"]["resolution"]) == (
        "/compute/v1/runs",
        "hs_mesh",
        128,
    )
    assert (transition_route, transition["kind"], transition["payload"]["transitionFrom"], transition["payload"]["iterations"]) == (
        "/compute/v1/runs",
        "transition_mesh",
        "a",
        100,
    )
