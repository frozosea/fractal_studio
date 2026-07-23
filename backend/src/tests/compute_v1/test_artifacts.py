from __future__ import annotations

from .client import ComputeClient, artifact_with_media_type
from .payloads import map_payload


def render_png(client: ComputeClient) -> dict[str, object]:
    manifest = client.completed_manifest("map_image", map_payload())
    return artifact_with_media_type(manifest, "image/png")


def test_artifact_download_returns_complete_png(client: ComputeClient) -> None:
    png = render_png(client)

    result = client.artifact(str(png["artifactId"]))

    assert result.status == 200
    assert result.headers.get("Content-Type") == "image/png"
    assert result.content.startswith(b"\x89PNG\r\n\x1a\n")


def test_artifact_range_returns_requested_png_signature(client: ComputeClient) -> None:
    png = render_png(client)

    result = client.artifact(str(png["artifactId"]), byte_range="bytes=0-7")

    assert result.status == 206
    assert result.content == b"\x89PNG\r\n\x1a\n"
    assert result.headers.get("Content-Range", "").startswith("bytes 0-7/")
