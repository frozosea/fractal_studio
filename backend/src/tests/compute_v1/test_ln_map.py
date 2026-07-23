from __future__ import annotations

import pytest

from .client import ComputeClient, artifact_with_media_type
from .payloads import builtin_formula, ln_map_payload, sequence_program


@pytest.mark.parametrize("color_mode", ["escape", "hist_eq"])
def test_sequence_ln_map_produces_image_and_sidecar(
    client: ComputeClient, color_mode: str,
) -> None:
    payload = ln_map_payload(color_mode=color_mode, orbit=sequence_program())

    manifest = client.completed_manifest("ln_map", payload)

    media_types = {item["mediaType"] for item in manifest["artifacts"]}
    assert manifest["escapeAnalysis"]["certifiedRadius"] == 2.0
    assert media_types >= {"image/png", "application/json"}


def test_builtin_orbit_ln_map_matches_legacy_png(client: ComputeClient) -> None:
    legacy = client.completed_manifest("ln_map", ln_map_payload())
    orbit = client.completed_manifest(
        "ln_map", ln_map_payload(orbit=builtin_formula()),
    )

    legacy_png = artifact_with_media_type(legacy, "image/png")
    orbit_png = artifact_with_media_type(orbit, "image/png")
    assert legacy_png["sha256"] == orbit_png["sha256"]


def test_ln_map_run_is_created_asynchronously(client: ComputeClient) -> None:
    run_id, response = client.create_run("ln_map", ln_map_payload())

    assert response.json()["data"]["status"] == "queued"
    assert client.wait_for_run(run_id)["status"] == "completed"


def test_ln_map_manifest_contains_kernel_execution_evidence(client: ComputeClient) -> None:
    manifest = client.completed_manifest(
        "ln_map", ln_map_payload(orbit=sequence_program()),
    )

    execution = manifest["hardwareExecution"]
    assert execution["kernelReported"] is True
    assert execution["hardwareClass"] == "cpu"
    assert execution["actualEngine"] == "openmp_orbit"
    assert execution["actualScalar"] == "fp64"
