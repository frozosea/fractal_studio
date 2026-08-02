"""Stateful Compute routing, scheduling, probing, and artifact rewriting."""

from __future__ import annotations

import asyncio
import hashlib
import json
import re
from collections import defaultdict
from datetime import UTC, datetime, timedelta
from typing import Any
from uuid import UUID

from sqlalchemy import delete, func, select, update
from sqlalchemy.ext.asyncio import AsyncSession, async_sessionmaker

from app.config import Settings, get_settings
from app.errors import GatewayError
from app.models import ComputeNode, ComputeRun, NodeProbe, RunArtifact
from app.schemas import NodeUpsertInput
from app.upstream import ComputeNodeClient, UpstreamError

ACTIVE_RUN_STATES = frozenset({"allocating", "queued", "running"})
TERMINAL_RUN_STATES = frozenset({"completed", "failed", "cancelled", "node_lost"})
SAFE_ARTIFACT_FILE = re.compile(r"^[^/\\:.][^/\\]*$")
NODE_KEY = re.compile(r"^[a-z0-9][a-z0-9-]{1,62}$")
# Compute emits manifest sidecars (progress, parameters, diagnostics) alongside
# user-facing binaries. Keep an explicit allow-list: the Gateway must not turn an
# arbitrary upstream content type into a downloadable public artifact.
ALLOWED_MEDIA_TYPES = frozenset(
    {
        "image/png",
        "video/mp4",
        "model/gltf-binary",
        "model/stl",
        "application/sla",
        "application/json",
        "application/octet-stream",
    }
)


def _now() -> datetime:
    return datetime.now(UTC)


def _request_hash(envelope: dict[str, object]) -> str:
    stable = {"kind": envelope.get("kind"), "payload": envelope.get("payload")}
    body = json.dumps(stable, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
    return hashlib.sha256(body.encode()).hexdigest()


def _require_envelope(envelope: dict[str, object], *, durable: bool) -> tuple[str, str | None]:
    if envelope.get("schemaVersion") != 1:
        raise GatewayError(422, "COMPUTE_VALIDATION_ERROR", "schemaVersion must equal 1")
    kind = envelope.get("kind")
    payload = envelope.get("payload")
    if not isinstance(kind, str) or not 1 <= len(kind) <= 64 or not isinstance(payload, dict):
        raise GatewayError(422, "COMPUTE_VALIDATION_ERROR", "invalid Compute envelope")
    key = envelope.get("idempotencyKey")
    if durable and (not isinstance(key, str) or not 1 <= len(key) <= 200):
        raise GatewayError(422, "COMPUTE_VALIDATION_ERROR", "idempotencyKey must contain 1..200 characters")
    return kind, key if isinstance(key, str) else None


def _node_supports(node: ComputeNode, *, kind: str, payload: dict[str, object], persistent: bool) -> bool:
    caps = node.capabilities_json or {}
    kinds = caps.get("persistentKinds" if persistent else "previewKinds")
    if not isinstance(kinds, list) or kind not in kinds:
        return False
    jobs = caps.get("jobs")
    job = next((item for item in jobs if isinstance(item, dict) and item.get("kind") == kind), None) if isinstance(jobs, list) else None
    if job is None:
        return False
    engine = payload.get("engine")
    engines = job.get("engines")
    if isinstance(engine, str) and engine not in {"auto", "cpu"} and isinstance(engines, list) and engine not in engines:
        return False
    scalar = payload.get("scalarType")
    scalars = job.get("scalars")
    mapped_scalar = {"float": "fp32", "double": "fp64", "long_double": "fp80"}.get(str(scalar), scalar)
    return not (
        isinstance(mapped_scalar, str)
        and mapped_scalar != "auto"
        and isinstance(scalars, list)
        and mapped_scalar not in scalars
    )


class GatewayService:
    def __init__(
        self,
        session_factory: async_sessionmaker[AsyncSession],
        *,
        upstream: ComputeNodeClient | None = None,
        settings: Settings | None = None,
    ) -> None:
        self._sessions = session_factory
        self._settings = settings or get_settings()
        self._upstream = upstream or ComputeNodeClient(self._settings)
        self._preview_in_flight: dict[UUID, int] = defaultdict(int)
        self._preview_lock = asyncio.Lock()

    async def upsert_node(self, node_key: str, payload: NodeUpsertInput) -> dict[str, object]:
        if not NODE_KEY.fullmatch(node_key):
            raise GatewayError(422, "COMPUTE_VALIDATION_ERROR", "invalid node key")
        base_url = str(payload.base_url).rstrip("/")
        async with self._sessions() as session, session.begin():
            node = (
                await session.execute(select(ComputeNode).where(ComputeNode.node_key == node_key).with_for_update())
            ).scalar_one_or_none()
            if node is None:
                node = ComputeNode(node_key=node_key, base_url=base_url)
                session.add(node)
            node.base_url = base_url
            node.max_durable_slots = payload.max_durable_slots
            node.max_preview_slots = payload.max_preview_slots
            node.state = "offline" if payload.enabled else "disabled"
        if payload.enabled:
            await self.probe_node(node_key, activate_on_success=True)
        return await self.node_view(node_key)

    async def set_node_state(self, node_key: str, state: str) -> dict[str, object]:
        if state not in {"active", "draining", "disabled"}:
            raise ValueError("invalid node state")
        if state == "active":
            await self.probe_node(node_key, activate_on_success=True)
        else:
            async with self._sessions() as session, session.begin():
                node = await self._locked_node(session, node_key)
                node.state = state
        return await self.node_view(node_key)

    async def node_view(self, node_key: str) -> dict[str, object]:
        async with self._sessions() as session:
            node = (
                await session.execute(select(ComputeNode).where(ComputeNode.node_key == node_key))
            ).scalar_one_or_none()
            if node is None:
                raise GatewayError(404, "COMPUTE_NODE_NOT_FOUND", "Compute node not found")
            used = await self._active_run_count(session, node.id)
            return self._node_view(node, used)

    async def list_nodes(self) -> list[dict[str, object]]:
        async with self._sessions() as session:
            nodes = (await session.execute(select(ComputeNode).order_by(ComputeNode.node_key))).scalars().all()
            return [self._node_view(node, await self._active_run_count(session, node.id)) for node in nodes]

    async def probe_node(self, node_key: str, *, activate_on_success: bool = False) -> None:
        async with self._sessions() as session:
            node = (
                await session.execute(select(ComputeNode).where(ComputeNode.node_key == node_key))
            ).scalar_one_or_none()
            if node is None:
                raise GatewayError(404, "COMPUTE_NODE_NOT_FOUND", "Compute node not found")
        try:
            latency = await self._upstream.health(node)
            capabilities = await self._upstream.capabilities(node)
        except UpstreamError as error:
            await self._record_probe(node.id, healthy=False, latency_ms=None, error_code=error.code)
            if activate_on_success:
                raise self._unavailable_error(error)
            return
        async with self._sessions() as session, session.begin():
            locked = await self._locked_node(session, node_key)
            locked.last_healthy_at = _now()
            locked.capabilities_json = capabilities
            locked.capabilities_at = _now()
            if (activate_on_success or locked.state == "offline") and locked.state != "draining":
                locked.state = "active"
            session.add(NodeProbe(node_id=locked.id, healthy=True, latency_ms=latency))

    async def probe_all(self) -> None:
        async with self._sessions() as session:
            keys = (await session.execute(select(ComputeNode.node_key).where(ComputeNode.state.in_(("active", "draining", "offline"))))).scalars().all()
        for key in keys:
            await self.probe_node(key)
        cutoff = _now() - timedelta(seconds=self._settings.node_offline_after_seconds)
        async with self._sessions() as session, session.begin():
            nodes = (await session.execute(select(ComputeNode).where(ComputeNode.state == "active").with_for_update())).scalars().all()
            for node in nodes:
                if node.last_healthy_at is None or node.last_healthy_at < cutoff:
                    node.state = "offline"
                    await session.execute(
                        update(ComputeRun)
                        .where(ComputeRun.node_id == node.id, ComputeRun.state.in_(ACTIVE_RUN_STATES))
                        .values(state="node_lost", terminal_at=_now())
                    )
            await session.execute(delete(NodeProbe).where(NodeProbe.checked_at < _now() - timedelta(days=7)))

    async def capabilities(self) -> dict[str, object]:
        nodes = await self._eligible_nodes(kind=None, payload={}, persistent=False)
        if not nodes:
            raise GatewayError(503, "COMPUTE_CAPACITY_EXHAUSTED", "no healthy Compute node is available")
        values = [node.capabilities_json or {} for node in nodes]
        persistent = sorted({item for caps in values for item in caps.get("persistentKinds", []) if isinstance(item, str)})
        previews = sorted({item for caps in values for item in caps.get("previewKinds", []) if isinstance(item, str)})
        jobs: dict[str, dict[str, object]] = {}
        for caps in values:
            for item in caps.get("jobs", []):
                if isinstance(item, dict) and isinstance(item.get("kind"), str):
                    jobs.setdefault(str(item["kind"]), item)
        async with self._sessions() as session:
            ready = False
            for node in nodes:
                if (
                    await self._active_run_count(session, node.id) < node.max_durable_slots
                    or self._preview_in_flight[node.id] < node.max_preview_slots
                ):
                    ready = True
                    break
        return {
            "schemaVersion": 1,
            "rendererVersion": "gateway",
            "persistentKinds": persistent,
            "previewKinds": previews,
            "jobs": list(jobs.values()),
            "gateway": {"healthyNodes": len(nodes), "ready": ready},
        }

    async def preview(self, envelope: dict[str, object]) -> tuple[bytes, str, dict[str, str]]:
        kind, _ = _require_envelope(envelope, durable=False)
        payload = envelope["payload"]
        assert isinstance(payload, dict)
        nodes = await self._eligible_nodes(kind=kind, payload=payload, persistent=False)
        async with self._preview_lock:
            available = [node for node in nodes if self._preview_in_flight[node.id] < node.max_preview_slots]
            if not available:
                raise GatewayError(503, "COMPUTE_CAPACITY_EXHAUSTED", "no Compute preview capacity is available")
            node = min(available, key=lambda item: (self._preview_in_flight[item.id] / item.max_preview_slots, item.node_key))
            self._preview_in_flight[node.id] += 1
        try:
            return await self._upstream.preview(node, envelope)
        except UpstreamError as error:
            raise self._unavailable_error(error) from error
        finally:
            async with self._preview_lock:
                self._preview_in_flight[node.id] = max(0, self._preview_in_flight[node.id] - 1)

    async def create_run(self, envelope: dict[str, object]) -> dict[str, object]:
        kind, key = _require_envelope(envelope, durable=True)
        assert key is not None
        request_hash = _request_hash(envelope)
        run = await self._reserve_or_replay(envelope, kind=kind, idempotency_key=key, request_hash=request_hash)
        if run.node_run_id is None:
            run = await self._submit_allocating(run.gateway_run_id)
        return self._run_envelope(run)

    async def get_run(self, gateway_run_id: UUID) -> dict[str, object]:
        run, node = await self._load_route(gateway_run_id)
        if run.state == "node_lost":
            raise GatewayError(502, "COMPUTE_NODE_LOST", "assigned Compute node is permanently unavailable")
        if run.node_run_id is None:
            run = await self._submit_allocating(gateway_run_id)
            return self._run_envelope(run)
        try:
            reply = await self._upstream.run_status(node, run.node_run_id)
        except UpstreamError as error:
            raise await self._handle_route_error(run, error) from error
        data = self._response_data(reply.body)
        await self._save_status(gateway_run_id, data)
        refreshed, _ = await self._load_route(gateway_run_id)
        return self._run_envelope(refreshed, data)

    async def cancel_run(self, gateway_run_id: UUID) -> dict[str, object]:
        run, node = await self._load_route(gateway_run_id)
        if run.state == "node_lost":
            raise GatewayError(502, "COMPUTE_NODE_LOST", "assigned Compute node is permanently unavailable")
        if run.node_run_id is None:
            raise GatewayError(503, "COMPUTE_NODE_UNAVAILABLE", "Compute run is still being submitted")
        try:
            reply = await self._upstream.cancel_run(node, run.node_run_id)
        except UpstreamError as error:
            raise await self._handle_route_error(run, error) from error
        data = self._response_data(reply.body)
        data["computeRunId"] = str(gateway_run_id)
        return {"schemaVersion": 1, "data": data}

    async def manifest(self, gateway_run_id: UUID) -> dict[str, object]:
        run, node = await self._load_route(gateway_run_id)
        if run.state == "node_lost":
            raise GatewayError(502, "COMPUTE_NODE_LOST", "assigned Compute node is permanently unavailable")
        if not run.node_run_id:
            raise GatewayError(409, "COMPUTE_INVALID_STATE", "Compute run is not completed")
        try:
            reply = await self._upstream.manifest(node, run.node_run_id)
        except UpstreamError as error:
            raise await self._handle_route_error(run, error) from error
        body = dict(reply.body)
        if body.get("status") != "completed":
            raise GatewayError(409, "COMPUTE_INVALID_STATE", "Compute run is not completed")
        artifacts = body.get("artifacts")
        if not isinstance(artifacts, list):
            raise GatewayError(502, "COMPUTE_INVALID_RESPONSE", "invalid Compute manifest")
        rewritten = await self._persist_manifest(gateway_run_id, run.node_run_id, artifacts)
        body["computeRunId"] = str(gateway_run_id)
        body["artifacts"] = rewritten
        return body

    async def stream_artifact(
        self, external_artifact_id: str, byte_range: str | None
    ) -> tuple[int, str, dict[str, str], Any]:
        async with self._sessions() as session:
            artifact = (
                await session.execute(
                    select(RunArtifact, ComputeNode)
                    .join(ComputeRun, ComputeRun.gateway_run_id == RunArtifact.gateway_run_id)
                    .join(ComputeNode, ComputeNode.id == ComputeRun.node_id)
                    .where(RunArtifact.external_artifact_id == external_artifact_id)
                )
            ).one_or_none()
        if artifact is None:
            raise GatewayError(404, "COMPUTE_ARTIFACT_NOT_FOUND", "Compute artifact not found")
        item, node = artifact
        try:
            return await self._upstream.stream_artifact(node, item.node_artifact_id, byte_range)
        except UpstreamError as error:
            raise self._unavailable_error(error) from error

    async def _reserve_or_replay(
        self, envelope: dict[str, object], *, kind: str, idempotency_key: str, request_hash: str
    ) -> ComputeRun:
        payload = envelope["payload"]
        assert isinstance(payload, dict)
        async with self._sessions() as session, session.begin():
            existing = (
                await session.execute(
                    select(ComputeRun).where(ComputeRun.idempotency_key == idempotency_key).with_for_update()
                )
            ).scalar_one_or_none()
            if existing:
                if existing.request_sha256 != request_hash:
                    raise GatewayError(409, "IDEMPOTENCY_CONFLICT", "idempotencyKey was reused with a different Compute request")
                return existing
            nodes = (
                await session.execute(
                    select(ComputeNode).where(ComputeNode.state == "active").with_for_update(skip_locked=True)
                )
            ).scalars().all()
            candidates: list[tuple[float, ComputeNode]] = []
            for node in nodes:
                if not self._healthy(node) or not _node_supports(node, kind=kind, payload=payload, persistent=True):
                    continue
                used = await self._active_run_count(session, node.id)
                if used < node.max_durable_slots:
                    candidates.append((used / node.max_durable_slots, node))
            if not candidates:
                raise GatewayError(503, "COMPUTE_CAPACITY_EXHAUSTED", "no healthy compatible Compute node has capacity")
            _, node = min(candidates, key=lambda item: (item[0], item[1].last_assigned_at or datetime.min.replace(tzinfo=UTC), item[1].node_key))
            node.last_assigned_at = _now()
            run = ComputeRun(
                idempotency_key=idempotency_key,
                request_sha256=request_hash,
                node_id=node.id,
                kind=kind,
                request_json=envelope,
                state="allocating",
            )
            session.add(run)
            await session.flush()
            return run

    async def _submit_allocating(self, gateway_run_id: UUID) -> ComputeRun:
        run, node = await self._load_route(gateway_run_id)
        if run.node_run_id:
            return run
        try:
            reply = await self._upstream.create_run(node, run.request_json)
        except UpstreamError as error:
            raise await self._handle_route_error(run, error) from error
        data = self._response_data(reply.body)
        node_run_id = data.get("computeRunId")
        if not isinstance(node_run_id, str) or not node_run_id:
            raise GatewayError(502, "COMPUTE_INVALID_RESPONSE", "Compute create response has no run ID")
        async with self._sessions() as session, session.begin():
            locked = (
                await session.execute(
                    select(ComputeRun).where(ComputeRun.gateway_run_id == gateway_run_id).with_for_update()
                )
            ).scalar_one()
            if locked.node_run_id is None:
                locked.node_run_id = node_run_id
                locked.last_status_json = data
                locked.state = self._state_from_status(data.get("status"))
            return locked

    async def _load_route(self, gateway_run_id: UUID) -> tuple[ComputeRun, ComputeNode]:
        async with self._sessions() as session:
            pair = (
                await session.execute(
                    select(ComputeRun, ComputeNode)
                    .join(ComputeNode, ComputeNode.id == ComputeRun.node_id)
                    .where(ComputeRun.gateway_run_id == gateway_run_id)
                )
            ).one_or_none()
        if pair is None:
            raise GatewayError(404, "COMPUTE_RUN_NOT_FOUND", "Compute run not found")
        return pair

    async def _save_status(self, gateway_run_id: UUID, data: dict[str, object]) -> None:
        async with self._sessions() as session, session.begin():
            run = (
                await session.execute(
                    select(ComputeRun).where(ComputeRun.gateway_run_id == gateway_run_id).with_for_update()
                )
            ).scalar_one()
            run.last_status_json = data
            run.state = self._state_from_status(data.get("status"))
            if run.state in TERMINAL_RUN_STATES and run.terminal_at is None:
                run.terminal_at = _now()

    async def _persist_manifest(
        self, gateway_run_id: UUID, node_run_id: str, artifacts: list[object]
    ) -> list[dict[str, object]]:
        rewritten: list[dict[str, object]] = []
        async with self._sessions() as session, session.begin():
            for item in artifacts:
                if not isinstance(item, dict):
                    raise GatewayError(502, "COMPUTE_INVALID_RESPONSE", "invalid Compute artifact")
                node_id = item.get("artifactId")
                media_type = item.get("mediaType")
                size = item.get("sizeBytes")
                sha256 = item.get("sha256")
                if not isinstance(node_id, str) or not node_id.startswith(f"{node_run_id}:"):
                    raise GatewayError(502, "COMPUTE_INVALID_RESPONSE", "invalid Compute artifact ID")
                filename = node_id.removeprefix(f"{node_run_id}:")
                if (
                    not SAFE_ARTIFACT_FILE.fullmatch(filename)
                    or media_type not in ALLOWED_MEDIA_TYPES
                    or not isinstance(size, int)
                    or not 0 < size <= 524_288_000
                    or not isinstance(sha256, str)
                    or not re.fullmatch(r"[0-9a-f]{64}", sha256)
                ):
                    raise GatewayError(502, "COMPUTE_INVALID_RESPONSE", "unsafe Compute artifact manifest")
                external_id = f"{gateway_run_id}:{filename}"
                existing = (
                    await session.execute(
                        select(RunArtifact).where(RunArtifact.external_artifact_id == external_id).with_for_update()
                    )
                ).scalar_one_or_none()
                if existing is None:
                    session.add(
                        RunArtifact(
                            gateway_run_id=gateway_run_id,
                            external_artifact_id=external_id,
                            node_artifact_id=node_id,
                            media_type=media_type,
                            size_bytes=size,
                            sha256=sha256,
                        )
                    )
                result = dict(item)
                result["artifactId"] = external_id
                result["contentPath"] = f"/compute/v1/artifacts?artifactId={external_id}"
                rewritten.append(result)
        return rewritten

    async def _eligible_nodes(
        self, *, kind: str | None, payload: dict[str, object], persistent: bool
    ) -> list[ComputeNode]:
        async with self._sessions() as session:
            nodes = (await session.execute(select(ComputeNode).where(ComputeNode.state == "active"))).scalars().all()
        if kind is None:
            return [node for node in nodes if self._healthy(node)]
        return [node for node in nodes if self._healthy(node) and _node_supports(node, kind=kind, payload=payload, persistent=persistent)]

    async def _active_run_count(self, session: AsyncSession, node_id: UUID) -> int:
        return int(
            (await session.execute(
                select(func.count(ComputeRun.gateway_run_id)).where(
                    ComputeRun.node_id == node_id, ComputeRun.state.in_(ACTIVE_RUN_STATES)
                )
            )).scalar_one()
        )

    async def _locked_node(self, session: AsyncSession, node_key: str) -> ComputeNode:
        node = (
            await session.execute(select(ComputeNode).where(ComputeNode.node_key == node_key).with_for_update())
        ).scalar_one_or_none()
        if node is None:
            raise GatewayError(404, "COMPUTE_NODE_NOT_FOUND", "Compute node not found")
        return node

    async def _record_probe(
        self, node_id: UUID, *, healthy: bool, latency_ms: int | None, error_code: str | None
    ) -> None:
        async with self._sessions() as session, session.begin():
            session.add(NodeProbe(node_id=node_id, healthy=healthy, latency_ms=latency_ms, error_code=error_code))

    def _node_view(self, node: ComputeNode, used_durable_slots: int) -> dict[str, object]:
        return {
            "nodeKey": node.node_key,
            "state": node.state,
            "maxDurableSlots": node.max_durable_slots,
            "usedDurableSlots": used_durable_slots,
            "maxPreviewSlots": node.max_preview_slots,
            "healthy": self._healthy(node),
            "lastHealthyAt": node.last_healthy_at,
        }

    def _healthy(self, node: ComputeNode) -> bool:
        return node.last_healthy_at is not None and node.last_healthy_at >= _now() - timedelta(
            seconds=self._settings.node_offline_after_seconds
        )

    @staticmethod
    def _state_from_status(value: object) -> str:
        status = str(value)
        return status if status in {"queued", "running", "completed", "failed", "cancelled"} else "failed"

    @staticmethod
    def _response_data(body: dict[str, object]) -> dict[str, object]:
        data = body.get("data")
        if not isinstance(data, dict):
            raise GatewayError(502, "COMPUTE_INVALID_RESPONSE", "invalid Compute response")
        return dict(data)

    def _run_envelope(self, run: ComputeRun, data: dict[str, object] | None = None) -> dict[str, object]:
        result = dict(data or run.last_status_json or {})
        result["computeRunId"] = str(run.gateway_run_id)
        result["status"] = result.get("status", run.state if run.state != "allocating" else "queued")
        result["progress"] = result.get("progress", {"percent": 0})
        # Local artifact IDs must not leave the Gateway. Manifest supplies rewritten IDs.
        result["artifacts"] = []
        return {"schemaVersion": 1, "data": result}

    async def _handle_route_error(self, run: ComputeRun, error: UpstreamError) -> GatewayError:
        if error.code == "COMPUTE_RUN_NOT_FOUND":
            async with self._sessions() as session, session.begin():
                locked = (
                    await session.execute(
                        select(ComputeRun).where(ComputeRun.gateway_run_id == run.gateway_run_id).with_for_update()
                    )
                ).scalar_one()
                locked.state = "node_lost"
                locked.terminal_at = _now()
            return GatewayError(502, "COMPUTE_NODE_LOST", "assigned Compute node lost its run")
        return self._unavailable_error(error)

    @staticmethod
    def _unavailable_error(error: UpstreamError) -> GatewayError:
        if error.code == "COMPUTE_UPSTREAM_AUTH_FAILED":
            return GatewayError(503, error.code, "Compute upstream authentication failed")
        if error.code == "COMPUTE_REJECTED":
            return GatewayError(error.status_code or 422, error.code, "Compute rejected request")
        if error.code == "COMPUTE_CONFLICT":
            return GatewayError(409, error.code, "Compute request conflicts with existing state")
        return GatewayError(503, "COMPUTE_NODE_UNAVAILABLE", "assigned Compute node is temporarily unavailable")
