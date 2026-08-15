"""Gateway state-machine integration tests against an explicitly configured PostgreSQL DB."""

from __future__ import annotations

import asyncio
import hashlib
import os
from collections.abc import AsyncIterator
from uuid import UUID

import pytest

TEST_DATABASE_URL = os.getenv("GATEWAY_TEST_DATABASE_URL")
pytestmark = pytest.mark.skipif(not TEST_DATABASE_URL, reason="set GATEWAY_TEST_DATABASE_URL")

if TEST_DATABASE_URL:
    from sqlalchemy import delete, select
    from sqlalchemy.ext.asyncio import AsyncSession, async_sessionmaker, create_async_engine

    from app.config import Settings
    from app.errors import GatewayError
    from app.models import ComputeNode, ComputeRun, NodeProbe, RunArtifact
    from app.schemas import NodeUpsertInput
    from app.services import GatewayService
    from app.upstream import UpstreamError, UpstreamReply


class FakeNodeClient:
    def __init__(self) -> None:
        self.created: list[tuple[str, str]] = []
        self._runs: dict[tuple[str, str], str] = {}
        self._counter = 0
        self.block_previews = False
        self.preview_started = asyncio.Event()
        self.release_previews = asyncio.Event()

    async def health(self, _: ComputeNode) -> int:
        return 1

    async def capabilities(self, _: ComputeNode) -> dict[str, object]:
        return {
            "schemaVersion": 1,
            "rendererVersion": "test-renderer-sm61",
            "hardware": {
                "cpu": {
                    "logicalCores": 8,
                    "physicalCores": 4,
                    "openmp": {"compiled": True, "runtime": True},
                    "avx2": {"compiled": True, "runtime": True},
                    "avx512": {"compiled": False, "runtime": False},
                },
                "cuda": {
                    "name": "Test GPU",
                    "runtime": True,
                    "computeCapability": {"major": 6, "minor": 1},
                    "totalVramBytes": 2_000_000_000,
                    "freeVramBytes": 1_000_000_000,
                }
            },
            "persistentKinds": ["map_image"],
            "previewKinds": ["map_image"],
            "builtInVariants": ["mandelbrot", "tricorn"],
            "axisTransitionVariants": ["mandelbrot"],
            "jobs": [{"kind": "map_image", "engines": ["openmp", "cuda"], "scalars": ["fp32", "fp64"]}],
            "coloring": {
                "builtInColorMaps": ["classic_cos", "viridis", "spectral1530"],
                "staticImageColorModes": ["direct", "eq_full", "eq_center"],
                "customGradient": True,
                "customGradientKinds": ["map_image"],
                "customGradientMaxStops": 16,
            },
            "orbitPrograms": {"formula": True, "sequence": True},
        }

    async def preview(
        self, node: ComputeNode, _: dict[str, object]
    ) -> tuple[bytes, str, dict[str, str]]:
        assert node.node_key in {"node-a", "node-b"}
        if self.block_previews:
            self.preview_started.set()
            await self.release_previews.wait()
        return b"preview", "application/octet-stream", {"X-FSD-Pixel-Format": "rgba8"}

    async def create_run(self, node: ComputeNode, envelope: dict[str, object]) -> UpstreamReply:
        key = str(envelope["idempotencyKey"])
        if key == "platform-job:rejected":
            raise UpstreamError("COMPUTE_REJECTED", status_code=422)
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


async def _add_node(
    gateway: GatewayService,
    key: str,
    slots: int = 1,
    preview_slots: int = 2,
    *,
    cpu_slots: int | None = None,
    gpu_slots: int | None = None,
    cpu_preview_slots: int | None = None,
    gpu_preview_slots: int | None = None,
) -> None:
    await gateway.upsert_node(
        key,
        NodeUpsertInput.model_validate(
            {
                "baseUrl": f"http://{key}.internal:18080",
                "maxDurableSlots": slots,
                "maxPreviewSlots": preview_slots,
                "maxCpuSlots": cpu_slots or slots,
                "maxGpuSlots": gpu_slots or slots,
                "maxCpuPreviewSlots": cpu_preview_slots or preview_slots,
                "maxGpuPreviewSlots": gpu_preview_slots or preview_slots,
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
    assert capabilities["builtInVariants"] == ["mandelbrot", "tricorn"]
    assert capabilities["axisTransitionVariants"] == ["mandelbrot"]
    assert capabilities["coloring"] == {
        "builtInColorMaps": ["classic_cos", "viridis", "spectral1530"],
        "staticImageColorModes": ["direct", "eq_full", "eq_center"],
        "customGradient": True,
        "customGradientKinds": ["map_image"],
        "customGradientMaxStops": 16,
    }
    assert capabilities["orbitPrograms"] == {"formula": True, "sequence": True}
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

    await service.reconcile_active_runs()
    nodes = {item["nodeKey"]: item for item in await service.list_nodes()}
    assert nodes["node-a"]["usedDurableSlots"] == 0
    assert nodes["node-b"]["usedDurableSlots"] == 0
    assert nodes["node-a"]["usedPreviewSlots"] == 0
    assert nodes["node-a"]["rendererVersion"] == "test-renderer-sm61"
    assert nodes["node-a"]["cpu"] == {
        "logicalCores": 8,
        "physicalCores": 4,
        "openmp": {"compiled": True, "runtime": True},
        "avx2": {"compiled": True, "runtime": True},
        "avx512": {"compiled": False, "runtime": False},
    }
    assert nodes["node-a"]["cpuAllocation"] == {
        "usedSlots": 0, "maxSlots": 1, "usedPreviewSlots": 0, "maxPreviewSlots": 2,
    }
    assert nodes["node-a"]["gpuAllocation"] == {
        "usedSlots": 0, "maxSlots": 1, "usedPreviewSlots": 0, "maxPreviewSlots": 2,
    }
    assert nodes["node-a"]["gpu"] == {
        "name": "Test GPU",
        "runtime": True,
        "computeCapability": {"major": 6, "minor": 1},
        "totalVramBytes": 2_000_000_000,
        "freeVramBytes": 1_000_000_000,
    }

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


async def test_concurrent_requests_use_separate_nodes(gateway: tuple[GatewayService, FakeNodeClient]) -> None:
    service, fake = gateway
    await _add_node(service, "node-a")
    await _add_node(service, "node-b")

    await asyncio.gather(
        service.create_run(_request("platform-job:concurrent-a")),
        service.create_run(_request("platform-job:concurrent-b")),
    )

    assert {node for node, _ in fake.created} == {"node-a", "node-b"}


async def test_preview_waits_for_a_slot_before_rejecting(gateway: tuple[GatewayService, FakeNodeClient]) -> None:
    service, fake = gateway
    await _add_node(service, "node-a", preview_slots=1)
    fake.block_previews = True

    first = asyncio.create_task(service.preview(_request("preview-first")))
    await fake.preview_started.wait()
    second = asyncio.create_task(service.preview(_request("preview-second")))
    await asyncio.sleep(0)
    assert not second.done()

    fake.release_previews.set()
    assert (await first)[0] == b"preview"
    assert (await second)[0] == b"preview"


async def test_rejected_run_releases_the_reserved_slot(gateway: tuple[GatewayService, FakeNodeClient]) -> None:
    service, _ = gateway
    await _add_node(service, "node-a")

    with pytest.raises(GatewayError) as raised:
        await service.create_run(_request("platform-job:rejected"))

    assert raised.value.status_code == 422
    assert (await service.list_nodes())[0]["usedDurableSlots"] == 0


async def test_cpu_and_gpu_runs_use_independent_capacity_pools(
    gateway: tuple[GatewayService, FakeNodeClient],
) -> None:
    service, fake = gateway
    await _add_node(service, "node-a", cpu_slots=2, gpu_slots=1)
    cpu_one = _request("platform-job:cpu-one")
    cpu_one["payload"]["engine"] = "cpu"
    cpu_two = _request("platform-job:cpu-two")
    cpu_two["payload"]["engine"] = "cpu"
    gpu = _request("platform-job:gpu")
    gpu["payload"]["engine"] = "cuda"

    await service.create_run(cpu_one)
    await service.create_run(cpu_two)
    await service.create_run(gpu)

    assert fake.created == [
        ("node-a", "platform-job:cpu-one"),
        ("node-a", "platform-job:cpu-two"),
        ("node-a", "platform-job:gpu"),
    ]
    view = (await service.list_nodes())[0]
    assert view["cpuAllocation"]["usedSlots"] == 2
    assert view["gpuAllocation"]["usedSlots"] == 1


async def test_auto_request_reserves_both_resource_pools(
    gateway: tuple[GatewayService, FakeNodeClient],
) -> None:
    service, fake = gateway
    await _add_node(service, "node-a", cpu_slots=2, gpu_slots=1)

    auto = _request("platform-job:auto")
    auto["payload"]["engine"] = "auto"
    await service.create_run(auto)

    gpu = _request("platform-job:gpu-blocked")
    gpu["payload"]["engine"] = "cuda"
    with pytest.raises(GatewayError) as raised:
        await service.create_run(gpu)
    assert raised.value.status_code == 503
    assert raised.value.code == "COMPUTE_CAPACITY_EXHAUSTED"

    cpu = _request("platform-job:cpu-spare")
    cpu["payload"]["engine"] = "cpu"
    await service.create_run(cpu)

    assert fake.created == [
        ("node-a", "platform-job:auto"),
        ("node-a", "platform-job:cpu-spare"),
    ]
    view = (await service.list_nodes())[0]
    assert view["cpuAllocation"]["usedSlots"] == 2
    assert view["gpuAllocation"]["usedSlots"] == 1


async def test_preview_pools_are_independent_by_resource(
    gateway: tuple[GatewayService, FakeNodeClient],
) -> None:
    service, fake = gateway
    await _add_node(service, "node-a", cpu_preview_slots=1, gpu_preview_slots=2)
    fake.block_previews = True

    cpu_first = _request("preview-cpu-first")
    cpu_first["payload"]["engine"] = "cpu"
    first = asyncio.create_task(service.preview(cpu_first))
    await fake.preview_started.wait()

    cpu_second = _request("preview-cpu-second")
    cpu_second["payload"]["engine"] = "cpu"
    waiting = asyncio.create_task(service.preview(cpu_second))
    await asyncio.sleep(0)
    assert not waiting.done()

    gpu_preview = _request("preview-gpu-free")
    gpu_preview["payload"]["engine"] = "cuda"
    gpu_task = asyncio.create_task(service.preview(gpu_preview))

    from sqlalchemy import select as sa_select

    deadline = asyncio.get_running_loop().time() + 5
    while True:
        async with service._sessions() as session:
            node = (await session.execute(sa_select(ComputeNode).where(ComputeNode.node_key == "node-a"))).scalar_one()
            in_flight = service._preview_in_flight
            if in_flight[node.id, "cpu"] == 1 and in_flight[node.id, "gpu"] == 1:
                break
        if asyncio.get_running_loop().time() > deadline:
            pytest.fail(f"preview slot never acquired; in_flight={dict(service._preview_in_flight)}")
        await asyncio.sleep(0.01)

    fake.release_previews.set()
    assert (await first)[0] == b"preview"
    assert (await waiting)[0] == b"preview"
    assert (await gpu_task)[0] == b"preview"


async def test_reconciliation_resubmits_an_allocating_run(
    gateway: tuple[GatewayService, FakeNodeClient],
) -> None:
    service, fake = gateway
    await _add_node(service, "node-a")
    request = _request("platform-job:recovery")
    await service._reserve_or_replay(
        request,
        kind="map_image",
        idempotency_key="platform-job:recovery",
        request_hash=hashlib.sha256(b"recovery").hexdigest(),
    )

    await service.reconcile_active_runs()

    assert fake.created == [("node-a", "platform-job:recovery")]


async def test_draining_node_never_accepts_a_new_run(gateway: tuple[GatewayService, FakeNodeClient]) -> None:
    service, fake = gateway
    await _add_node(service, "node-a")
    await _add_node(service, "node-b")
    await service.set_node_state("node-a", "draining")

    await service.create_run(_request("platform-job:drain"))
    assert fake.created == [("node-b", "platform-job:drain")]


async def test_zero_nodes_are_healthy_but_capacity_endpoints_fail_without_reservation(
    gateway: tuple[GatewayService, FakeNodeClient],
) -> None:
    service, _ = gateway
    operations = (
        service.capabilities,
        lambda: service.preview(_request("preview-offline")),
        lambda: service.create_run(_request("platform-job:offline")),
    )
    for operation in operations:
        with pytest.raises(GatewayError) as raised:
            await operation()
        assert raised.value.status_code == 503
        assert raised.value.code == "COMPUTE_CAPACITY_EXHAUSTED"
    async with service._sessions() as session:
        assert await service._active_resource_counts(session, UUID(int=0)) == {"cpu": 0, "gpu": 0}


async def test_bootstrap_updates_address_without_overwriting_operator_state(
    gateway: tuple[GatewayService, FakeNodeClient],
) -> None:
    service, _ = gateway
    await _add_node(service, "node-a")
    await service.set_node_state("node-a", "draining")
    payload = NodeUpsertInput.model_validate(
        {
            "baseUrl": "http://10.66.0.2:18080",
            "maxDurableSlots": 1,
            "maxPreviewSlots": 2,
            "enabled": True,
        }
    )
    view = await service.bootstrap_node("node-a", payload)
    assert view["state"] == "draining"
    async with service._sessions() as session:
        node = (
            await session.execute(select(ComputeNode).where(ComputeNode.node_key == "node-a"))
        ).scalar_one()
        assert node.base_url == "http://10.66.0.2:18080"

    await service.set_node_state("node-a", "disabled")
    assert (await service.bootstrap_node("node-a", payload))["state"] == "disabled"
