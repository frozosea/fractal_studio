from __future__ import annotations

from .client import ComputeClient
from .payloads import transition_mesh_payload


def test_transition_mesh_run_is_asynchronous(client: ComputeClient) -> None:
    run_id, response = client.create_run("transition_mesh", transition_mesh_payload())

    assert response.json()["data"]["status"] == "queued"
    assert client.wait_for_run(run_id)["status"] == "completed"


def test_transition_mesh_manifest_reports_artifacts_and_hardware(client: ComputeClient) -> None:
    manifest = client.completed_manifest("transition_mesh", transition_mesh_payload())

    media_types = {item["mediaType"] for item in manifest["artifacts"]}
    execution = manifest["hardwareExecution"]
    assert media_types >= {"model/gltf-binary", "application/sla"}
    assert execution["kernelReported"] is True
    assert execution["actualEngine"] == "openmp_fp32"
    assert execution["actualScalar"] == "fp32"
    assert execution["hardwareClass"] == "cpu"


def test_legacy_transition_mesh_remains_synchronous(client: ComputeClient) -> None:
    result = client.request(
        "/api/transition/mesh", body=transition_mesh_payload(), authorized=False,
    )

    assert result.status == 200
    assert result.json()["status"] == "completed"
    assert result.json()["vertexCount"] > 0
