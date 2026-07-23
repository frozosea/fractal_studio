"""Contract-compatible private Compute stub for local M2 T05/T06 E2E runs only."""

from __future__ import annotations

from dataclasses import dataclass
from uuid import uuid4

from fastapi import FastAPI, Header, HTTPException, Response


app = FastAPI()
_SERVICE_KEY = "Bearer test-compute-key"


@dataclass
class _Run:
    client_job_id: str
    output_kind: str
    polls: int = 0
    cancelled: bool = False
    create_attempts: int = 0


_runs_by_id: dict[str, _Run] = {}
_run_id_by_client_job: dict[str, str] = {}
_transient_attempts: dict[str, int] = {}


def _require_service_key(authorization: str | None) -> None:
    if authorization != _SERVICE_KEY:
        raise HTTPException(status_code=401)


@app.post("/api/map/render-inline")
async def render_inline(
    payload: dict[str, object], authorization: str | None = Header(default=None)
) -> Response:
    _require_service_key(authorization)
    width = int(payload["width"])
    height = int(payload["height"])
    rgba = bytes([32, 128, 255, 255]) * (width * height)
    return Response(
        content=rgba,
        media_type="application/octet-stream",
        headers={
            "X-FSD-Width": str(width),
            "X-FSD-Height": str(height),
            "X-FSD-Pixel-Format": "rgba8",
        },
    )


@app.post("/api/map/render")
@app.post("/api/video/export")
@app.post("/api/hs/mesh")
@app.post("/api/transition/mesh")
async def create_durable(payload: dict[str, object], authorization: str | None = Header(default=None)) -> dict[str, object]:
    _require_service_key(authorization)
    client_job_id = str(payload["clientJobId"])
    if client_job_id in _run_id_by_client_job:
        run_id = _run_id_by_client_job[client_job_id]
        return {"runId": run_id, "clientJobId": client_job_id, "status": "queued"}
    if payload.get("variant") == "compute_rejected":
        raise HTTPException(status_code=503)
    if payload.get("variant") == "transient_failure":
        _transient_attempts[client_job_id] = _transient_attempts.get(client_job_id, 0) + 1
        if _transient_attempts[client_job_id] == 1:
            raise HTTPException(status_code=503)
    route = "image"
    if "durationSeconds" in payload:
        route = "video"
    elif "transitionFrom" in payload:
        route = "transition_mesh"
    elif "resolution" in payload:
        route = "hs_mesh"
    run_id = f"run-{uuid4().hex}"
    _run_id_by_client_job[client_job_id] = run_id
    _runs_by_id[run_id] = _Run(client_job_id=client_job_id, output_kind=route)
    return {"runId": run_id, "clientJobId": client_job_id, "status": "queued"}


@app.get("/api/runs/status")
async def run_status(runId: str, authorization: str | None = Header(default=None)) -> dict[str, object]:
    _require_service_key(authorization)
    run = _runs_by_id.get(runId)
    if run is None:
        raise HTTPException(status_code=404)
    if run.cancelled:
        return _run_body(runId, run, status="cancelled", progress=0, artifacts=[])
    run.polls += 1
    if run.polls == 1:
        return _run_body(runId, run, status="running", progress=50, artifacts=[])
    media_type = {
        "image": "image/png",
        "video": "video/mp4",
        "hs_mesh": "model/gltf-binary",
        "transition_mesh": "model/gltf-binary",
    }[run.output_kind]
    return _run_body(
        runId,
        run,
        status="completed",
        progress=100,
        artifacts=[
            {
                "artifactId": f"{runId}:master",
                "purpose": "master",
                "mediaType": media_type,
                "sizeBytes": 64,
            }
        ],
    )


@app.post("/api/runs/cancel")
async def cancel_run(payload: dict[str, object], authorization: str | None = Header(default=None)) -> dict[str, object]:
    _require_service_key(authorization)
    run_id = str(payload["runId"])
    run = _runs_by_id.get(run_id)
    if run is None:
        raise HTTPException(status_code=404)
    run.cancelled = True
    return {"runId": run_id, "status": "cancel_requested"}


def _run_body(
    run_id: str, run: _Run, *, status: str, progress: int, artifacts: list[dict[str, object]]
) -> dict[str, object]:
    return {
        "runId": run_id,
        "clientJobId": run.client_job_id,
        "status": status,
        "progressPercent": progress,
        "artifacts": artifacts,
    }
