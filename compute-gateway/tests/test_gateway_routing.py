"""Gateway state-machine integration tests against an explicitly configured PostgreSQL DB."""

from __future__ import annotations

import hashlib
import os
from collections.abc import AsyncIterator
from uuid import UUID

import pytest

TEST_DATABASE_URL = os.getenv("GATEWAY_TEST_DATABASE_URL")
pytestmark = pytest.mark.skipif(not TEST_DATABASE_URL, reason="set GATEWAY_TEST_DATABASE_URL")

if TEST_DATABASE_URL:
    from sqlalchemy import delete
    from sqlalchemy.ext.asyncio import AsyncSession, async_sessionmaker, create_async_engine

    from app.config import Settings
    from app.errors import GatewayError
    from app.models import ComputeNode, ComputeRun, NodeProbe, RunArtifact
    from app.schemas import NodeUpsertInput
    from app.services import GatewayService
    from app.upstream import UpstreamReply


class FakeNodeClient:
    def __init__(self) -> None:
        self.created: list[tuple[str, str]] = []
        self._runs: dict[tuple[str, str], str] = {}
        self._counter = 0

    async def health(self, _: ComputeNode) -> int:
        return 1

    async def capabilities(self, _: ComputeNode) -> dict[str, object]:
        return {
            "schemaVersion": 1,
            "persistentKinds": ["map_image"],
            "previewKinds": ["map_image"],
            "jobs": [{"kind": "map_image", "engines": ["openmp", "cuda"], "scalars": ["fp32", "fp64"]}],
        }

    async def preview(
        self, node: ComputeNode, _: dict[str, object]
    ) -> tuple[bytes, str, dict[str, str]]:
        assert node.node_key in {"node-a", "node-b"}
        return b"preview", "application/octet-stream", {"X-FSD-Pixel-Format": "rgba8"}

    async def create_run(self, node: ComputeNode, envelope: dict[str, object]) -> UpstreamReply:
        key = str(envelope["idempotencyKey"])
        route_key = (node.node_key, key)
        if route_key not in self._runs:
            self._counter += 1
            self._runs[route_key] = f"{node.node_key}-run-{self._counter}"
            self.created.append(route_key)
        return UpstreamReply(
            202,
            {
                "schemaVersion": 1,
                "data": {
                    "computeRunId": self._runs[route_key],
                    "status": "queued",
                    "progress": {"percent": 0},
                    "artifacts": [],
                },
            },
            {},
        )

    async def run_status(self, node: ComputeNode, node_run_id: str) -> UpstreamReply:
        return UpstreamReply(
            200,
            {
                "schemaVersion": 1,
                "data": {
                    "computeRunId": node_run_id,
                    "status": "completed",
                    "progress": {"percent": 100},
                    "artifacts": [],
                },
            },
            {},
        )

    async def cancel_run(self, _: ComputeNode, node_run_id: str) -> UpstreamReply:
        return UpstreamReply(
            202,
            {"schemaVersion": 1, "data": {"computeRunId": node_run_id, "status": "running", "accepted": True, "cancelRequested": True}},
            {},
        )

    async def manifest(self, _: ComputeNode, node_run_id: str) -> UpstreamReply:
        content = b"gateway-artifact"
        progress = b'{"status":"completed"}'
        return UpstreamReply(
            200,
            {
                "computeRunId": node_run_id,
                "status": "completed",
                "artifacts": [
                    {
                        "artifactId": f"{node_run_id}:master.png",
                        "name": "master.png",
                        "kind": "image",
                        "mediaType": "image/png",
                        "sizeBytes": len(content),
                        "sha256": hashlib.sha256(content).hexdigest(),
                    },
                    {
                        "artifactId": f"{node_run_id}:progress.json",
                        "name": "progress.json",
                        "kind": "report",
                        "mediaType": "application/json",
                        "sizeBytes": len(progress),
                        "sha256": hashlib.sha256(progress).hexdigest(),
                    }
                ],
            },
            {},
        )

    async def stream_artifact(
        self, _: ComputeNode, node_artifact_id: str, __: str | None
    ) -> tuple[int, str, dict[str, str], AsyncIterator[bytes]]:
        assert node_artifact_id.endswith(":master.png")

        async def chunks() -> AsyncIterator[bytes]:
            yield b"gateway-artifact"

        return 200, "image/png", {"content-length": "16"}, chunks()


@pytest.fixture
async def gateway() -> AsyncIterator[tuple[GatewayService, FakeNodeClient]]:
    assert TEST_DATABASE_URL
    assert TEST_DATABASE_URL.rstrip("/").endswith("/compute_gateway_test"), "use isolated compute_gateway_test DB"
    engine = create_async_engine(TEST_DATABASE_URL)
    sessions = async_sessionmaker(engine, expire_on_commit=False, class_=AsyncSession)
    async with engine.begin() as connection:
        for model in (RunArtifact, ComputeRun, NodeProbe, ComputeNode):
            await connection.execute(delete(model))
    fake = FakeNodeClient()
    settings = Settings(
        database_url=TEST_DATABASE_URL,
        compute_gateway_service_key="test-gateway-key-1234",
        compute_gateway_admin_key="test-admin-key-123456",
        compute_upstream_service_key="test-upstream-key-123",
    )
    yield GatewayService(sessions, upstream=fake, settings=settings), fake
    async with engine.begin() as connection:
        for model in (RunArtifact, ComputeRun, NodeProbe, ComputeNode):
            await connection.execute(delete(model))
    await engine.dispose()


async def _add_node(gateway: GatewayService, key: str, slots: int = 1) -> None:
    await gateway.upsert_node(
        key,
        NodeUpsertInput.model_validate(
            {
                "baseUrl": f"http://{key}.internal:18080",
                "maxDurableSlots": slots,
                "maxPreviewSlots": 2,
                "enabled": True,
            }
        ),
    )


def _request(key: str, width: int = 64) -> dict[str, object]:
    return {
        "schemaVersion": 1,
        "kind": "map_image",
        "idempotencyKey": key,
        "payload": {"width": width, "height": 64, "engine": "auto", "scalarType": "auto"},
    }


async def test_sticky_routing_replay_manifest_and_artifact_stream(
    gateway: tuple[GatewayService, FakeNodeClient],
) -> None:
    service, fake = gateway
    await _add_node(service, "node-a")
    await _add_node(service, "node-b")
    capabilities = await service.capabilities()
    assert capabilities["gateway"] == {"healthyNodes": 2, "ready": True}
    preview, media_type, headers = await service.preview(_request("preview-no-key"))
    assert preview == b"preview"
    assert media_type == "application/octet-stream"
    assert headers["X-FSD-Pixel-Format"] == "rgba8"

    first = await service.create_run(_request("platform-job:one"))
    await service.create_run(_request("platform-job:two"))
    first_id = UUID(str(first["data"]["computeRunId"]))

    assert fake.created == [("node-a", "platform-job:one"), ("node-b", "platform-job:two")]
    replay = await service.create_run(_request("platform-job:one"))
    assert replay["data"]["computeRunId"] == str(first_id)
    assert fake.created == [("node-a", "platform-job:one"), ("node-b", "platform-job:two")]

    status = await service.get_run(first_id)
    assert status["data"]["computeRunId"] == str(first_id)
    assert status["data"]["status"] == "completed"
    assert status["data"]["artifacts"] == []

    manifest = await service.manifest(first_id)
    artifact_id = str(manifest["artifacts"][0]["artifactId"])
    assert artifact_id == f"{first_id}:master.png"
    assert manifest["artifacts"][1]["artifactId"] == f"{first_id}:progress.json"
    status_code, media_type, _, chunks = await service.stream_artifact(artifact_id, None)
    assert status_code == 200
    assert media_type == "image/png"
    assert b"".join([chunk async for chunk in chunks]) == b"gateway-artifact"


async def test_reused_idempotency_key_with_different_payload_conflicts(
    gateway: tuple[GatewayService, FakeNodeClient],
) -> None:
    service, _ = gateway
    await _add_node(service, "node-a")
    await service.create_run(_request("platform-job:one"))

    with pytest.raises(GatewayError) as raised:
        await service.create_run(_request("platform-job:one", width=128))
    assert raised.value.status_code == 409
    assert raised.value.code == "IDEMPOTENCY_CONFLICT"


async def test_draining_node_never_accepts_a_new_run(gateway: tuple[GatewayService, FakeNodeClient]) -> None:
    service, fake = gateway
    await _add_node(service, "node-a")
    await _add_node(service, "node-b")
    await service.set_node_state("node-a", "draining")

    await service.create_run(_request("platform-job:drain"))
    assert fake.created == [("node-b", "platform-job:drain")]
