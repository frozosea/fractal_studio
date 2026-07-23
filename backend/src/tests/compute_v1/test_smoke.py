from __future__ import annotations

from .client import ComputeClient, artifact_with_media_type
from .payloads import map_payload


def test_core_render_chain_produces_downloadable_png(client: ComputeClient) -> None:
    health = client.request("/compute/v1/health", authorized=False)
    assert health.status == 200
    assert health.json()["status"] == "ok"

    manifest = client.completed_manifest("map_image", map_payload())
    png = artifact_with_media_type(manifest, "image/png")
    download = client.artifact(png["artifactId"])

    assert download.status == 200
    assert download.content.startswith(b"\x89PNG\r\n\x1a\n")
