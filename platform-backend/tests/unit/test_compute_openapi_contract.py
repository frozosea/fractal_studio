"""Structural guard for the checked-in private Compute OpenAPI contract."""

from pathlib import Path

import yaml


SPEC = Path(__file__).parents[2] / "docs" / "compute-openapi.yaml"


def test_compute_openapi_describes_current_private_v1_surface() -> None:
    document = yaml.safe_load(SPEC.read_text())

    assert document["openapi"] == "3.1.0"
    paths = document["paths"]
    assert set(paths) == {
        "/compute/v1/health",
        "/compute/v1/capabilities",
        "/compute/v1/previews",
        "/compute/v1/runs",
        "/compute/v1/runs/{computeRunId}",
        "/compute/v1/runs/{computeRunId}/cancel",
        "/compute/v1/runs/{computeRunId}/manifest",
        "/compute/v1/artifacts",
    }
    assert paths["/compute/v1/health"]["get"]["security"] == []
    assert document["components"]["securitySchemes"]["ComputeServiceKey"] == {
        "type": "http", "scheme": "bearer", "bearerFormat": "opaque-service-key"
    }


def test_compute_openapi_models_worker_payload_union_and_real_minimums() -> None:
    schemas = yaml.safe_load(SPEC.read_text())["components"]["schemas"]
    request_variants = schemas["CreateRunRequest"]["oneOf"]
    assert {item["$ref"].rsplit("/", 1)[-1] for item in request_variants} == {
        "MapImageRunRequest", "ZoomVideoRunRequest", "HsMeshRunRequest", "TransitionMeshRunRequest"
    }
    assert schemas["IdempotentRequest"]["required"] == ["idempotencyKey"]
    assert schemas["MapPayload"]["properties"]["width"]["minimum"] == 64
    assert schemas["MapPayload"]["properties"]["height"]["minimum"] == 64
    assert schemas["VideoPayload"]["allOf"][1]["properties"]["width"]["minimum"] == 128
    assert schemas["VideoPayload"]["allOf"][1]["properties"]["height"]["minimum"] == 128
