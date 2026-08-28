"""ASGI application entry point."""

import logging
import secrets
import time
import uuid

from fastapi import FastAPI, HTTPException, Request
from fastapi.exceptions import RequestValidationError
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse

from app.admin.router import router as admin_router
from app.ai.listing_router import router as ai_listing_router
from app.ai.router import router as ai_router
from app.auth.router import router as auth_router
from app.assets.router import router as assets_router
from app.commerce.router import router as commerce_router
from app.finance.payout_operator_router import router as payout_operator_router
from app.finance.payout_router import router as payout_router
from app.marketplace.router import router as marketplace_router
from app.membership.router import router as membership_router
from app.core.config import get_settings
from app.core.logging import configure_logging, log_event
from app.core.request_context import idempotency_key_var, request_id_var, user_id_var
from app.studio.router import router as studio_router


_settings = get_settings()
# Keep interactive docs and the OpenAPI schema off the production surface so
# the full route surface is not publicly discoverable at /platform/docs.
app = FastAPI(
    title="Fractal Platform API",
    version="0.1.0",
    docs_url=None if _settings.app_env == "production" else "/docs",
    redoc_url=None if _settings.app_env == "production" else "/redoc",
    openapi_url=None if _settings.app_env == "production" else "/openapi.json",
)
configure_logging(json_output=_settings.log_json or _settings.app_env == "production")
app.add_middleware(
    CORSMiddleware,
    allow_origins=sorted(get_settings().trusted_origins),
    allow_credentials=True,
    allow_methods=["GET", "POST", "PATCH", "DELETE"],
    allow_headers=[
        "Authorization",
        "Content-Type",
        "Idempotency-Key",
        "X-CSRF-Token",
        "X-Request-ID",
    ],
    expose_headers=["X-Session-Token"],
)
app.include_router(auth_router)
app.include_router(ai_router)
app.include_router(ai_listing_router)
app.include_router(studio_router)
app.include_router(assets_router)
app.include_router(marketplace_router)
app.include_router(commerce_router)
app.include_router(payout_router)
app.include_router(payout_operator_router)
app.include_router(membership_router)
app.include_router(admin_router)


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


def _append_vary(response, field: str) -> None:
    existing = response.headers.get("Vary")
    if existing is None:
        response.headers["Vary"] = field
        return
    if field.lower() in {part.strip().lower() for part in existing.split(",")}:
        return
    response.headers["Vary"] = f"{existing}, {field}"


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
        # Every response is session-scoped: never let a browser or intermediary
        # replay one account's body to the next account signed in on the same device.
        response.headers.setdefault("Cache-Control", "no-store")
        _append_vary(response, "Cookie")
        _append_vary(response, "Authorization")
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


_DEFAULT_ERROR_CODE = {
    401: "unauthenticated",
    402: "payment_required",
    403: "forbidden",
    404: "not_found",
    409: "invalid_state",
    413: "payload_too_large",
    422: "validation_error",
    429: "quota_exceeded",
    502: "compute_error",
    503: "payment_unavailable",
}

_PUBLIC_DETAIL_CODES = {
    "AI_DISABLED",
    "AI_PROVIDER_UNAVAILABLE",
    "AI_TRIAL_EXHAUSTED",
    "ai_concurrency_exhausted",
    "ai_conversation_not_found",
    "ai_image_dimensions_invalid",
    "ai_image_invalid",
    "ai_message_not_found",
    "ai_message_not_complete",
    "ai_request_in_progress",
    "COMPUTE_CAPACITY_EXHAUSTED",
    "account_disabled",
    "creator_name_change_too_soon",
    "email_already_registered",
    "export_quota_exhausted",
    "handle_already_registered",
    "idempotency_conflict",
    "insufficient_creator_balance",
    "cannot_disable_self",
    "cannot_remove_own_admin",
    "admin_creator_role_conflict",
    "admin_scope_only",
    "last_admin",
    "payout_request_pending",
    "preview_not_found",
    "preview_not_ready",
    "preview_queue_unavailable",
    "asset_already_owned",
}


@app.exception_handler(HTTPException)
async def platform_http_exception_handler(request: Request, error: HTTPException) -> JSONResponse:
    detail = error.detail if isinstance(error.detail, str) else None
    code = (
        detail
        if detail in _PUBLIC_DETAIL_CODES
        else _DEFAULT_ERROR_CODE.get(error.status_code, "request_failed")
    )
    message = code.replace("_", " ")
    return JSONResponse(
        status_code=error.status_code,
        content={"error": {"code": code, "message": message, "details": {}}},
    )


@app.exception_handler(RequestValidationError)
async def platform_validation_exception_handler(
    request: Request, error: RequestValidationError
) -> JSONResponse:
    return JSONResponse(
        status_code=422,
        content={
            "error": {
                "code": "validation_error",
                "message": "request validation failed",
                "details": {},
            }
        },
    )


@app.exception_handler(Exception)
async def platform_unexpected_exception_handler(request: Request, error: Exception) -> JSONResponse:
    log_event(logging.ERROR, "unhandled request error", error_type=type(error).__name__)
    return JSONResponse(
        status_code=500,
        content={
            "error": {"code": "internal_error", "message": "internal server error", "details": {}}
        },
    )


@app.get("/healthz", tags=["system"])
async def healthz() -> dict[str, str]:
    """Container liveness endpoint; does not expose dependencies or secrets."""
    return {"status": "ok"}
