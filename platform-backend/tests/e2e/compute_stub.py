"""Small C++ Compute v1-compatible stub for Platform E2E only."""

from __future__ import annotations

import hashlib
from dataclasses import dataclass
from uuid import uuid4

from fastapi import FastAPI, Header, HTTPException, Response


app = FastAPI()
_SERVICE_KEY = "Bearer test-compute-key"


@dataclass
class _Run:
    compute_kind: str
    polls: int = 0
    cancelled: bool = False


_runs: dict[str, _Run] = {}
_run_by_idempotency_key: dict[str, str] = {}
_transient_attempts: dict[str, int] = {}
_MASTER_BYTES = b"fractal-compute-stub-master" * 3
_MASTER_SHA256 = hashlib.sha256(_MASTER_BYTES).hexdigest()


def _require_service_key(authorization: str | None) -> None:
    if authorization != _SERVICE_KEY:
        raise HTTPException(status_code=401, detail="COMPUTE_UNAUTHORIZED")


def _payload(envelope: dict[str, object]) -> tuple[str, dict[str, object]]:
    if envelope.get("schemaVersion") != 1 or not isinstance(envelope.get("kind"), str):
        raise HTTPException(status_code=400)
    payload = envelope.get("payload")
    if not isinstance(payload, dict):
        raise HTTPException(status_code=400)
    return str(envelope["kind"]), payload


def _master_filename(run: _Run) -> str:
    return {
        "map_image": "master.png",
        "zoom_video": "master.mp4",
        "hs_mesh": "master.glb",
        "transition_mesh": "master.glb",
    }[run.compute_kind]


@app.post("/compute/v1/previews")
async def render_inline(
    envelope: dict[str, object], authorization: str | None = Header(default=None)
) -> Response:
    _require_service_key(authorization)
    kind, payload = _payload(envelope)
    if kind != "map_image":
        raise HTTPException(status_code=422)
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


@app.post("/compute/v1/runs", status_code=202)
async def create_durable(
    envelope: dict[str, object], authorization: str | None = Header(default=None)
) -> dict[str, object]:
    _require_service_key(authorization)
    kind, payload = _payload(envelope)
    key = str(envelope.get("idempotencyKey", ""))
    if not key:
        raise HTTPException(status_code=400)
    if key in _run_by_idempotency_key:
        return _created_body(_run_by_idempotency_key[key], _runs[_run_by_idempotency_key[key]])
    if payload.get("variant") == "compute_rejected":
        raise HTTPException(status_code=503)
    if payload.get("variant") == "transient_failure":
        _transient_attempts[key] = _transient_attempts.get(key, 0) + 1
        if _transient_attempts[key] == 1:
            raise HTTPException(status_code=503)
    run_id = f"run-{uuid4().hex}"
    _run_by_idempotency_key[key] = run_id
    _runs[run_id] = _Run(compute_kind=kind)
    return _created_body(run_id, _runs[run_id])


@app.get("/compute/v1/runs/{run_id}")
async def run_status(run_id: str, authorization: str | None = Header(default=None)) -> dict[str, object]:
    _require_service_key(authorization)
    run = _runs.get(run_id)
    if run is None:
        raise HTTPException(status_code=404)
    if run.cancelled:
        return _status_body(run_id, run, status="cancelled", progress=0)
    run.polls += 1
    if run.polls == 1:
        return _status_body(run_id, run, status="running", progress=50)
    return _status_body(run_id, run, status="completed", progress=100)


@app.get("/compute/v1/runs/{run_id}/manifest")
async def manifest(run_id: str, authorization: str | None = Header(default=None)) -> dict[str, object]:
    _require_service_key(authorization)
    run = _runs.get(run_id)
    if run is None:
        raise HTTPException(status_code=404)
    if run.cancelled:
        return {"schemaVersion": 1, "computeRunId": run_id, "status": "cancelled", "artifacts": []}
    if run.polls < 2:
        return {"schemaVersion": 1, "computeRunId": run_id, "status": "running", "artifacts": []}
    media_type = {
        "map_image": "image/png",
        "zoom_video": "video/mp4",
        "hs_mesh": "model/gltf-binary",
        "transition_mesh": "model/gltf-binary",
    }[run.compute_kind]
    filename = _master_filename(run)
    return {
        "schemaVersion": 1,
        "computeRunId": run_id,
        "status": "completed",
        "rendererVersion": "compute-stub",
        "artifacts": [
            {
                "artifactId": f"{run_id}:{filename}",
                "name": filename,
                "kind": "image" if media_type == "image/png" else "video" if media_type == "video/mp4" else "mesh",
                "mediaType": media_type,
                "sizeBytes": len(_MASTER_BYTES),
                "sha256": _MASTER_SHA256,
                "contentPath": f"/compute/v1/artifacts?artifactId={run_id}:{filename}",
            }
        ],
    }


@app.get("/compute/v1/artifacts")
async def artifact(artifactId: str, authorization: str | None = Header(default=None)) -> Response:
    _require_service_key(authorization)
    run_id, separator, name = artifactId.partition(":")
    if not separator or run_id not in _runs or name != _master_filename(_runs[run_id]):
        raise HTTPException(status_code=404)
    return Response(content=_MASTER_BYTES, media_type="application/octet-stream")


@app.post("/compute/v1/runs/{run_id}/cancel", status_code=202)
async def cancel_run(
    run_id: str, _payload_body: dict[str, object], authorization: str | None = Header(default=None)
) -> dict[str, object]:
    _require_service_key(authorization)
    run = _runs.get(run_id)
    if run is None:
        raise HTTPException(status_code=404)
    run.cancelled = True
    return {"schemaVersion": 1, "data": {"computeRunId": run_id, "status": "running", "accepted": True, "cancelRequested": True}}


def _created_body(run_id: str, run: _Run) -> dict[str, object]:
    return {"schemaVersion": 1, "data": {"computeRunId": run_id, "kind": run.compute_kind, "status": "queued"}}


def _status_body(run_id: str, run: _Run, *, status: str, progress: int) -> dict[str, object]:
    return {
        "schemaVersion": 1,
        "data": {
            "computeRunId": run_id,
            "status": status,
            "progress": {"percent": progress},
            "artifacts": [],
        },
    }
