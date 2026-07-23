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


def test_idempotency_key_rejects_different_request(client: ComputeClient) -> None:
    key = f"pytest:conflict:{uuid.uuid4()}"
    first = client.envelope("map_image", map_payload())
    second = client.envelope("map_image", map_payload(iterations=33))
    first["idempotencyKey"] = second["idempotencyKey"] = key

    created = client.request("/compute/v1/runs", body=first)
    conflict = client.request("/compute/v1/runs", body=second)

    assert created.status == 202
    assert conflict.status == 409
    assert conflict.json()["error"]["code"] == "IDEMPOTENCY_CONFLICT"


def test_map_run_manifest_contains_escape_certificate(client: ComputeClient) -> None:
    manifest = client.completed_manifest("map_image", map_payload(orbit=True))

    assert manifest["rendererVersion"] == "contract-pytest"
    assert manifest["escapeAnalysis"]["status"] == "certified_finite"
    assert manifest["escapeAnalysis"]["certifiedRadius"] == 2.0
    png = next(item for item in manifest["artifacts"] if item["mediaType"] == "image/png")
    assert len(png["sha256"]) == 64


def test_map_manifest_proves_openmp_execution(client: ComputeClient) -> None:
    manifest = client.completed_manifest("map_image", map_payload(orbit=True))

    execution = manifest["hardwareExecution"]
    assert execution["requestedEngine"] == "openmp"
    assert execution["actualEngine"] == "openmp"
    assert execution["hardwareClass"] == "cpu"
    assert execution["kernelReported"] is True
    assert execution["runtimeAvailable"] is True
    assert execution["engineFallback"] is False


def test_run_status_exposes_verified_hardware_execution(client: ComputeClient) -> None:
    run_id, _ = client.create_run("map_image", map_payload(orbit=True))

    terminal = client.wait_for_run(run_id)

    execution = terminal["hardwareExecution"]
    assert execution["actualEngine"] == "openmp"
    assert execution["hardwareClass"] == "cpu"
    assert execution["kernelReported"] is True
    assert execution["evidenceSource"] == "kernel_completion_telemetry"


def test_completed_map_reports_terminal_progress(client: ComputeClient) -> None:
    run_id, _ = client.create_run("map_image", map_payload())

    terminal = client.wait_for_run(run_id)

    assert terminal["progress"]["stage"] == "completed"
    assert terminal["progress"]["percent"] == 100.0
    assert terminal["progress"]["cancelable"] is False


def test_cuda_request_reports_real_gpu_or_explicit_fallback(client: ComputeClient) -> None:
    capabilities = client.request("/compute/v1/capabilities").json()
    manifest = client.completed_manifest("map_image", map_payload(engine="cuda"))

    execution = manifest["hardwareExecution"]
    assert execution["requestedEngine"] == "cuda"
    assert execution["kernelReported"] is True
    if capabilities["hardware"]["cuda"]["runtime"]:
        assert execution["hardwareClass"] in {"gpu", "hybrid"}
        assert "cuda" in execution["actualEngine"]
        assert execution["engineFallback"] is False
    else:
        assert execution["hardwareClass"] == "cpu"
        assert execution["engineFallback"] is True


def test_hs_mesh_run_manifest_contains_both_mesh_formats(client: ComputeClient) -> None:
    manifest = client.completed_manifest("hs_mesh", hs_mesh_payload())

    media_types = {item["mediaType"] for item in manifest["artifacts"]}
    assert manifest["status"] == "completed"
    assert manifest["escapeAnalysis"]["certifiedRadius"] == 2.0
    assert media_types >= {"model/gltf-binary", "application/sla"}


def test_hs_mesh_run_is_async_and_reports_hardware(client: ComputeClient) -> None:
    run_id, response = client.create_run("hs_mesh", hs_mesh_payload())

    assert response.json()["data"]["status"] == "queued"
    assert client.wait_for_run(run_id)["status"] == "completed"
    execution = client.manifest(run_id)["hardwareExecution"]
    assert execution["kernelReported"] is True
    assert execution["actualEngine"] == "openmp_orbit"
    assert execution["hardwareClass"] == "cpu"
