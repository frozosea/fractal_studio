"""Safe HTTP errors and exception handler."""

from __future__ import annotations

from typing import Any

from fastapi import FastAPI, Request
from fastapi.responses import JSONResponse


class GatewayError(RuntimeError):
    def __init__(self, status_code: int, code: str, message: str, details: dict[str, Any] | None = None) -> None:
        super().__init__(code)
        self.status_code = status_code
        self.code = code
        self.message = message
        self.details = details or {}


def install_error_handler(app: FastAPI) -> None:
    @app.exception_handler(GatewayError)
    async def handle_gateway_error(_: Request, error: GatewayError) -> JSONResponse:
        return JSONResponse(
            status_code=error.status_code,
            content={"error": {"code": error.code, "message": error.message, "details": error.details}},
        )
