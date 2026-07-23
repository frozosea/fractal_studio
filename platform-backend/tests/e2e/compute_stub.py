"""Minimal private Compute stub for local M2 E2E runs only."""

from __future__ import annotations

from fastapi import FastAPI, Header, HTTPException, Response


app = FastAPI()


@app.post("/api/map/render-inline")
async def render_inline(
    payload: dict[str, object], authorization: str | None = Header(default=None)
) -> Response:
    if authorization != "Bearer test-compute-key":
        raise HTTPException(status_code=401)
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
