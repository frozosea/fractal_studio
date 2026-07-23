from __future__ import annotations

import struct

from .client import ComputeClient, artifact_with_media_type
from .payloads import hs_field_payload


def test_hs_field_run_is_asynchronous(client: ComputeClient) -> None:
    run_id, response = client.create_run("hs_field", hs_field_payload())

    assert response.json()["data"]["status"] == "queued"
    assert client.wait_for_run(run_id)["status"] == "completed"


def test_hs_field_manifest_contains_data_metadata_and_hardware(client: ComputeClient) -> None:
    manifest = client.completed_manifest("hs_field", hs_field_payload())

    media_types = {item["mediaType"] for item in manifest["artifacts"]}
    execution = manifest["hardwareExecution"]
    assert media_types >= {"application/octet-stream", "application/json"}
    assert execution["kernelReported"] is True
    assert execution["actualEngine"] == "openmp_orbit"
    assert execution["hardwareClass"] == "cpu"


def test_hs_field_binary_artifact_contains_float64_grid(client: ComputeClient) -> None:
    manifest = client.completed_manifest("hs_field", hs_field_payload())
    field = artifact_with_media_type(manifest, "application/octet-stream")

    download = client.artifact(field["artifactId"])

    values = struct.unpack("<64d", download.content)
    assert download.status == 200
    assert len(download.content) == 8 * 8 * 8
    assert all(0.0 <= value <= 2.0 for value in values)


def test_legacy_hs_field_keeps_inline_response(client: ComputeClient) -> None:
    result = client.request(
        "/api/hs/field", body=hs_field_payload(), authorized=False,
    )

    data = result.json()
    assert result.status == 200
    assert data["status"] == "completed"
    assert data["width"] == data["height"] == 8
    assert len(data["fieldB64"]) > 0
