from __future__ import annotations

import asyncio
import sys
import time
import types
from pathlib import Path
from types import SimpleNamespace

from .client import ComputeClient as _TestComputeClient
from .platform_payloads import platform_map, platform_preview


def load_platform_client() -> type:
    studio_root = Path(__file__).resolve().parents[4]
    platform_root = studio_root / "platform-backend"
    sys.path.insert(0, str(platform_root))
    # Import the collaborator's real transport class without pulling the whole
    # FastAPI settings stack into the C++ test environment. The client only
    # reads attributes from the supplied settings object.
    config_stub = types.ModuleType("app.core.config")
    config_stub.Settings = object
    config_stub.get_settings = lambda: None
    sys.modules["app.core.config"] = config_stub
    from app.infrastructure.compute.compute_client import ComputeClient

    return ComputeClient


def test_real_platform_compute_client_calls_cpp(client: _TestComputeClient) -> None:
    compute_client_type = load_platform_client()
    settings = SimpleNamespace(
        compute_base_url=client.base_url,
        compute_service_key=client.service_key,
        compute_connect_timeout_seconds=5,
        compute_read_timeout_seconds=30,
    )
    platform_client = compute_client_type(settings)

    preview_payload = platform_preview()
    preview_payload.update({"engine": "openmp", "scalarType": "fp64"})
    preview = client.envelope("map_image", preview_payload)
    frame = asyncio.run(platform_client.render_map_inline(preview, timeout_seconds=15))
    assert (frame.width, frame.height, len(frame.rgba)) == (64, 64, 64 * 64 * 4)

    durable_payload = platform_map()
    durable_payload.update({"engine": "openmp", "scalarType": "fp64"})
    durable = client.envelope("map_image", durable_payload)
    durable["idempotencyKey"] = f"platform-client:{durable_payload['clientJobId']}"
    status = asyncio.run(platform_client.create_durable_run(
        route="/compute/v1/runs", request_body=durable
    ))
    deadline = time.monotonic() + 15
    while status.status not in {"completed", "failed", "cancelled"}:
        assert time.monotonic() < deadline
        status = asyncio.run(platform_client.get_run_status(run_id=status.run_id))
        time.sleep(0.02)

    assert status.status == "completed"
    assert status.run_id
    manifest = asyncio.run(platform_client.get_run_manifest(run_id=status.run_id))
    assert manifest.status == "completed"
    assert {artifact.media_type for artifact in manifest.artifacts} >= {"image/png"}
