from __future__ import annotations

from .client import ComputeClient
from .payloads import zoom_payload


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
