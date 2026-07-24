"""ASGI application entry point."""

import logging
import secrets
import time
import uuid

from fastapi import FastAPI, HTTPException, Request
from fastapi.exceptions import RequestValidationError
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse

from app.auth.router import router as auth_router
from app.assets.router import router as assets_router
from app.core.config import get_settings
from app.core.logging import configure_logging, log_event
from app.core.request_context import idempotency_key_var, request_id_var, user_id_var
from app.studio.router import router as studio_router


app = FastAPI(title="Fractal Platform API", version="0.1.0")
configure_logging(json_output=get_settings().log_json or get_settings().app_env == "production")
app.add_middleware(
    CORSMiddleware,
    allow_origins=sorted(get_settings().trusted_origins),
    allow_credentials=True,
    allow_methods=["GET", "POST", "PATCH", "DELETE"],
    allow_headers=["Content-Type", "Idempotency-Key", "X-CSRF-Token", "X-Request-ID"],
)
app.include_router(auth_router)
app.include_router(studio_router)
app.include_router(assets_router)


def _uuid7() -> str:
    milliseconds = int(time.time() * 1000)
    value = (
        (milliseconds << 80)
        | (0x7 << 76)
        | (secrets.randbits(12) << 64)
        | (0b10 << 62)
        | secrets.randbits(62)
    )
    return str(uuid.UUID(int=value))


def _trusted_request_id(request: Request) -> str:
    supplied = request.headers.get("x-request-id")
    if get_settings().trust_request_id_header and supplied:
        try:
            parsed = uuid.UUID(supplied)
            if parsed.version == 7:
                return str(parsed)
        except ValueError:
            pass
    return _uuid7()


@app.middleware("http")
async def assign_request_id(request: Request, call_next):
    request_id = _trusted_request_id(request)
    request.state.request_id = request_id
    request_token = request_id_var.set(request_id)
    user_token = user_id_var.set("-")
    idempotency_token = idempotency_key_var.set(request.headers.get("idempotency-key", "-"))
    try:
        response = await call_next(request)
        response.headers["X-Request-ID"] = request_id
        log_event(
            logging.INFO,
            "http request completed",
            method=request.method,
            path=request.url.path,
            status=response.status_code,
        )
        return response
    finally:
        request_id_var.reset(request_token)
        user_id_var.reset(user_token)
        idempotency_key_var.reset(idempotency_token)


_DEFAULT_ERROR_CODE = {401: "unauthenticated", 403: "forbidden", 404: "not_found", 409: "invalid_state", 413: "payload_too_large", 422: "validation_error", 429: "quota_exceeded", 502: "compute_error", 503: "service_unavailable"}


@app.exception_handler(HTTPException)
async def platform_http_exception_handler(request: Request, error: HTTPException) -> JSONResponse:
    detail = error.detail if isinstance(error.detail, str) else None
    code = detail or _DEFAULT_ERROR_CODE.get(error.status_code, "request_failed")
    if code == "idempotency_conflict":
        message = "idempotency key conflicts with a different request"
    else:
        message = _DEFAULT_ERROR_CODE.get(error.status_code, "request failed").replace("_", " ")
    return JSONResponse(status_code=error.status_code, content={"error": {"code": code, "message": message, "details": {}}})


@app.exception_handler(RequestValidationError)
async def platform_validation_exception_handler(request: Request, error: RequestValidationError) -> JSONResponse:
    return JSONResponse(status_code=422, content={"error": {"code": "validation_error", "message": "request validation failed", "details": {}}})


@app.exception_handler(Exception)
async def platform_unexpected_exception_handler(request: Request, error: Exception) -> JSONResponse:
    log_event(logging.ERROR, "unhandled request error", error_type=type(error).__name__)
    return JSONResponse(status_code=500, content={"error": {"code": "internal_error", "message": "internal server error", "details": {}}})


@app.get("/healthz", tags=["system"])
async def healthz() -> dict[str, str]:
    """Container liveness endpoint; does not expose dependencies or secrets."""
    return {"status": "ok"}
