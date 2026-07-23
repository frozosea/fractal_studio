from __future__ import annotations

import uuid

from .client import ComputeClient
from .payloads import hs_mesh_payload, map_payload


def test_duplicate_idempotency_key_returns_same_run(client: ComputeClient) -> None:
    body = client.envelope("map_image", map_payload(orbit=True))
    body["idempotencyKey"] = f"pytest:duplicate:{uuid.uuid4()}"

    first = client.request("/compute/v1/runs", body=body)
    second = client.request("/compute/v1/runs", body=body)

    assert first.status == second.status == 202
    assert first.json()["data"]["computeRunId"] == second.json()["data"]["computeRunId"]


def test_map_run_manifest_contains_escape_certificate(client: ComputeClient) -> None:
    manifest = client.completed_manifest("map_image", map_payload(orbit=True))

    assert manifest["rendererVersion"] == "contract-pytest"
    assert manifest["escapeAnalysis"]["status"] == "certified_finite"
    assert manifest["escapeAnalysis"]["certifiedRadius"] == 2.0
    png = next(item for item in manifest["artifacts"] if item["mediaType"] == "image/png")
    assert len(png["sha256"]) == 64


def test_hs_mesh_run_manifest_contains_both_mesh_formats(client: ComputeClient) -> None:
    manifest = client.completed_manifest("hs_mesh", hs_mesh_payload())

    media_types = {item["mediaType"] for item in manifest["artifacts"]}
    assert manifest["status"] == "completed"
    assert manifest["escapeAnalysis"]["certifiedRadius"] == 2.0
    assert media_types >= {"model/gltf-binary", "application/sla"}
