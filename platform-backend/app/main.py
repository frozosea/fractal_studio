"""ASGI application entry point."""

import uuid

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware

from app.auth.router import router as auth_router
from app.core.config import get_settings
from app.core.request_context import request_id_var


app = FastAPI(title="Fractal Platform API", version="0.1.0")
app.add_middleware(
    CORSMiddleware,
    allow_origins=sorted(get_settings().trusted_origins),
    allow_credentials=True,
    allow_methods=["GET", "POST", "PATCH", "DELETE"],
    allow_headers=["Content-Type", "Idempotency-Key", "X-CSRF-Token", "X-Request-ID"],
)
app.include_router(auth_router)


@app.middleware("http")
async def assign_request_id(request, call_next):
    supplied = request.headers.get("x-request-id")
    try:
        request_id = str(uuid.UUID(supplied)) if supplied else str(uuid.uuid4())
    except ValueError:
        request_id = str(uuid.uuid4())
    request.state.request_id = request_id
    token = request_id_var.set(request_id)
    try:
        response = await call_next(request)
    finally:
        request_id_var.reset(token)
    response.headers["X-Request-ID"] = request_id
    return response


@app.get("/healthz", tags=["system"])
async def healthz() -> dict[str, str]:
    """Container liveness endpoint; does not expose dependencies or secrets."""
    return {"status": "ok"}
