from __future__ import annotations

import base64
import struct

from .client import ComputeClient
from .payloads import map_payload, strict_dsl_payload


def test_map_preview_returns_rgba_frame(client: ComputeClient) -> None:
    result = client.preview("map_image", map_payload())

    assert result.status == 200
    assert result.headers.get("Content-Type") == "application/octet-stream"
    assert len(result.content) == 64 * 64 * 4


def test_sequence_preview_reports_effective_engine(client: ComputeClient) -> None:
    result = client.preview("map_image", map_payload(orbit=True))

    assert result.status == 200
    assert result.headers.get("X-FSD-Engine") == "openmp"
    assert result.headers.get("X-FSD-Scalar") == "fp64"
    assert len(result.content) == 64 * 64 * 4


def test_unverified_dsl_raw_field_runs_to_iteration_limit(client: ComputeClient) -> None:
    result = client.preview("raw_field", strict_dsl_payload())

    assert result.status == 200
    data = result.json()["data"]
    assert struct.unpack("<I", base64.b64decode(data["iterB64"])) == (5,)
    assert base64.b64decode(data["orbitClassB64"]) == b"\x00"
    assert data["escapeAnalysis"]["status"] == "unverified"
    assert data["escapeAnalysis"]["certifiedRadius"] is None
