from __future__ import annotations

from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import JSONResponse

from app.core.config import get_settings
from app.core.http import request_id_middleware
from app.studio.router import router as studio_router


app = FastAPI(title="Fractal Studio Platform", version="0.1.0")
app.middleware("http")(request_id_middleware)
app.include_router(studio_router)


@app.get("/health/live")
async def live() -> dict:
    return {"status": "ok", "service": "fractal-studio-platform"}


@app.get("/health/ready")
async def ready() -> dict:
    settings = get_settings()
    return {
        "status": "ok",
        "environment": settings.environment,
        "foundationRoutesEnabled": settings.foundation_routes_enabled,
    }


@app.exception_handler(HTTPException)
async def http_error(_: Request, error: HTTPException) -> JSONResponse:
    if isinstance(error.detail, dict) and "code" in error.detail:
        payload = error.detail
    else:
        payload = {"code": "HTTP_ERROR", "message": str(error.detail), "details": {}}
    return JSONResponse(status_code=error.status_code, content={"error": payload})
