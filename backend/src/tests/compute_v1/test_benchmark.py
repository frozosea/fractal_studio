from __future__ import annotations

from .client import ComputeClient
from .payloads import benchmark_payload


def test_benchmark_run_is_asynchronous(client: ComputeClient) -> None:
    run_id, response = client.create_run("benchmark", benchmark_payload())

    assert response.json()["data"]["status"] == "queued"
    assert client.wait_for_run(run_id)["status"] == "completed"


def test_benchmark_manifest_contains_per_path_hardware_evidence(client: ComputeClient) -> None:
    manifest = client.completed_manifest("benchmark", benchmark_payload())

    execution = manifest["hardwareExecution"]
    paths = execution["paths"]
    assert execution["mode"] == "multi_path"
    assert execution["kernelReported"] is True
    assert execution["evidenceSource"] == "benchmark_candidate_telemetry"
    assert len(paths) >= 3
    assert all(path["requestedEngine"] and path["actualEngine"] for path in paths)
    assert all(path["requestedSampleCount"] == 1 for path in paths)
    assert "application/json" in {item["mediaType"] for item in manifest["artifacts"]}
