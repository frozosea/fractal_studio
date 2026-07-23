from __future__ import annotations

from .client import ComputeClient
from .payloads import special_points_enumerate_payload, special_points_search_payload


def test_special_points_enumeration_is_asynchronous(client: ComputeClient) -> None:
    run_id, response = client.create_run(
        "special_points_enumerate", special_points_enumerate_payload(),
    )

    assert response.json()["data"]["status"] == "queued"
    assert client.wait_for_run(run_id)["status"] == "completed"


def test_special_points_enumeration_manifest_has_report_and_hardware(client: ComputeClient) -> None:
    manifest = client.completed_manifest(
        "special_points_enumerate", special_points_enumerate_payload(),
    )

    execution = manifest["hardwareExecution"]
    assert "application/json" in {item["mediaType"] for item in manifest["artifacts"]}
    assert execution["kernelReported"] is True
    assert execution["actualEngine"] == "openmp"
    assert execution["actualScalar"] == "fp64"


def test_special_points_search_reports_hardware(client: ComputeClient) -> None:
    manifest = client.completed_manifest(
        "special_points_search", special_points_search_payload(),
    )

    execution = manifest["hardwareExecution"]
    assert "application/json" in {item["mediaType"] for item in manifest["artifacts"]}
    assert execution["kernelReported"] is True
    assert execution["hardwareClass"] == "cpu"
