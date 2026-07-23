from __future__ import annotations

from .client import ComputeClient
from .payloads import builtin_formula, zoom_payload


def reusable_zoom_payload() -> dict:
    return {**zoom_payload(), "depthOctaves": 1.0}


def test_zoom_video_preview_reports_final_frame_engine(client: ComputeClient) -> None:
    result = client.preview("video_preview", zoom_payload())

    assert result.status == 200
    data = result.json()["data"]
    assert data["status"] == "completed"
    assert data["finalFrameEngine"] == "openmp"
    assert data["finalFrameScalar"] == "fp64"


def test_zoom_video_run_produces_mp4(client: ComputeClient) -> None:
    manifest = client.completed_manifest("zoom_video", zoom_payload(), timeout=20.0)

    media_types = {item["mediaType"] for item in manifest["artifacts"]}
    assert manifest["escapeAnalysis"]["certifiedRadius"] == 2.0
    assert "video/mp4" in media_types
    assert manifest["hardwareExecution"]["kernelReported"] is True
    assert manifest["hardwareExecution"]["hardwareClass"] == "cpu"


def test_zoom_video_reuses_matching_orbit_ln_map(client: ComputeClient) -> None:
    source_id, _ = client.completed_run("ln_map", reusable_zoom_payload(), timeout=20.0)
    payload = {**reusable_zoom_payload(), "lnMapRunId": source_id}

    manifest = client.completed_manifest("zoom_video", payload, timeout=20.0)

    assert manifest["status"] == "completed"
    assert manifest["escapeAnalysis"]["certifiedRadius"] == 2.0


def test_zoom_video_rejects_mismatched_orbit_ln_map(client: ComputeClient) -> None:
    source_id, _ = client.completed_run("ln_map", reusable_zoom_payload(), timeout=20.0)
    payload = {**reusable_zoom_payload(), "lnMapRunId": source_id}
    payload["orbitProgram"] = builtin_formula()

    run_id, _ = client.create_run("zoom_video", payload)
    terminal = client.wait_for_run(run_id, timeout=20.0)

    assert terminal["status"] == "failed"


def test_legacy_zoom_uses_orbit_from_ln_map_sidecar(client: ComputeClient) -> None:
    source_id, _ = client.completed_run("ln_map", reusable_zoom_payload(), timeout=20.0)
    payload = {
        "lnMapArtifactId": f"{source_id}:ln_map.png",
        "width": 128, "height": 128, "fps": 1,
        "depthOctaves": 1.0, "secondsPerOctave": 1.0,
        "cudaWarp": False,
    }

    manifest = client.completed_manifest("legacy_zoom_video", payload, timeout=20.0)

    assert manifest["escapeAnalysis"]["certifiedRadius"] == 2.0
    assert manifest["hardwareExecution"]["actualEngine"] == "openmp"
    assert manifest["hardwareExecution"]["kernelReported"] is True
