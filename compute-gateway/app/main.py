"""Private Compute v1 Gateway ASGI application."""

from __future__ import annotations

import asyncio
from collections.abc import AsyncIterator
from contextlib import asynccontextmanager, suppress
from uuid import UUID

from fastapi import Depends, FastAPI, Header, Path
from fastapi.responses import Response, StreamingResponse

from app.config import get_settings
from app.db import SessionLocal
from app.errors import GatewayError, install_error_handler
from app.schemas import NodeUpsertInput
from app.security import require_admin_key, require_gateway_key
from app.services import GatewayService

settings = get_settings()
service = GatewayService(SessionLocal, settings=settings)


async def _probe_loop() -> None:
    await asyncio.sleep(1)
    while True:
        with suppress(Exception):
            await service.probe_all()
        await asyncio.sleep(settings.node_probe_interval_seconds)


async def _bootstrap_nodes() -> None:
    await asyncio.sleep(1)
    for item in settings.bootstrap_nodes:
        node_key = item.get("nodeKey")
        if not isinstance(node_key, str):
            continue
        with suppress(GatewayError):
            await service.bootstrap_node(node_key, NodeUpsertInput.model_validate(item))


@asynccontextmanager
async def lifespan(_: FastAPI) -> AsyncIterator[None]:
    bootstrap = asyncio.create_task(_bootstrap_nodes(), name="compute-gateway-bootstrap")
    task = asyncio.create_task(_probe_loop(), name="compute-gateway-probe")
    try:
        yield
    finally:
        bootstrap.cancel()
        task.cancel()
        with suppress(asyncio.CancelledError):
            await bootstrap
        with suppress(asyncio.CancelledError):
            await task


# Keep the interactive docs and OpenAPI schema off the production surface so
# the internal admin API is not publicly discoverable.
app = FastAPI(
    title="Fractal Studio Compute Gateway",
    version="0.1.0",
    lifespan=lifespan,
    docs_url=None if settings.app_env == "production" else "/docs",
    redoc_url=None if settings.app_env == "production" else "/redoc",
    openapi_url=None if settings.app_env == "production" else "/openapi.json",
)
install_error_handler(app)


@app.get("/compute/v1/health")
async def health() -> dict[str, object]:
    return {"schemaVersion": 1, "status": "ok", "service": "compute-gateway"}


@app.get("/compute/v1/capabilities", dependencies=[Depends(require_gateway_key)])
async def capabilities() -> dict[str, object]:
    return await service.capabilities()


@app.post("/compute/v1/previews", dependencies=[Depends(require_gateway_key)])
async def preview(envelope: dict[str, object]) -> Response:
    body, media_type, headers = await service.preview(envelope)
    return Response(content=body, media_type=media_type, headers=headers)


@app.post("/compute/v1/runs", status_code=202, dependencies=[Depends(require_gateway_key)])
async def create_run(envelope: dict[str, object]) -> dict[str, object]:
    return await service.create_run(envelope)


@app.get("/compute/v1/runs/{compute_run_id}", dependencies=[Depends(require_gateway_key)])
async def get_run(compute_run_id: UUID) -> dict[str, object]:
    return await service.get_run(compute_run_id)


@app.post("/compute/v1/runs/{compute_run_id}/cancel", status_code=202, dependencies=[Depends(require_gateway_key)])
async def cancel_run(compute_run_id: UUID, payload: dict[str, object]) -> dict[str, object]:
    if payload:
        raise GatewayError(422, "COMPUTE_VALIDATION_ERROR", "cancel request body must be empty")
    return await service.cancel_run(compute_run_id)


@app.get("/compute/v1/runs/{compute_run_id}/manifest", dependencies=[Depends(require_gateway_key)])
async def manifest(compute_run_id: UUID) -> dict[str, object]:
    return await service.manifest(compute_run_id)


@app.get("/compute/v1/artifacts", dependencies=[Depends(require_gateway_key)])
async def artifact(artifactId: str, range_header: str | None = Header(default=None, alias="Range")) -> StreamingResponse:
    status_code, media_type, headers, chunks = await service.stream_artifact(artifactId, range_header)
    return StreamingResponse(chunks, status_code=status_code, media_type=media_type, headers=headers)


@app.get("/internal/v1/nodes", dependencies=[Depends(require_admin_key)])
async def list_nodes() -> dict[str, object]:
    return {"data": await service.list_nodes()}


@app.put("/internal/v1/nodes/{node_key}", dependencies=[Depends(require_admin_key)])
async def upsert_node(node_key: str = Path(pattern=r"^[a-z0-9][a-z0-9-]{1,62}$"), payload: NodeUpsertInput = ...) -> dict[str, object]:
    return await service.upsert_node(node_key, payload)


@app.post("/internal/v1/nodes/{node_key}/drain", dependencies=[Depends(require_admin_key)])
async def drain_node(node_key: str = Path(pattern=r"^[a-z0-9][a-z0-9-]{1,62}$")) -> dict[str, object]:
    return await service.set_node_state(node_key, "draining")


@app.post("/internal/v1/nodes/{node_key}/activate", dependencies=[Depends(require_admin_key)])
async def activate_node(node_key: str = Path(pattern=r"^[a-z0-9][a-z0-9-]{1,62}$")) -> dict[str, object]:
    return await service.set_node_state(node_key, "active")


@app.post("/internal/v1/nodes/{node_key}/disable", dependencies=[Depends(require_admin_key)])
async def disable_node(node_key: str = Path(pattern=r"^[a-z0-9][a-z0-9-]{1,62}$")) -> dict[str, object]:
    return await service.set_node_state(node_key, "disabled")
