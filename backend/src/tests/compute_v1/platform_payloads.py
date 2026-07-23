from __future__ import annotations

from typing import Any
from uuid import uuid4


def platform_map(*, client_job_id: str | None = None) -> dict[str, Any]:
    return {
        "clientJobId": client_job_id or str(uuid4()),
        "width": 64,
        "height": 64,
        "iterations": 24,
        "variant": "mandelbrot",
        "centerRe": -0.75,
        "centerIm": 0.0,
        "scale": 3.0,
        "julia": False,
        "bailout": 2.0,
        "engine": "cpu",
        "scalarType": "double",
        "colorMap": "classic_cos",
        "stillExport": True,
        "background": True,
    }


def platform_preview() -> dict[str, Any]:
    payload = platform_map()
    payload.pop("clientJobId")
    payload.pop("stillExport")
    payload.pop("background")
    payload["requestId"] = str(uuid4())
    return payload


def platform_video() -> dict[str, Any]:
    payload = platform_map()
    payload.pop("stillExport")
    payload.pop("background")
    payload.update({"width": 128, "height": 128, "fps": 1, "durationSeconds": 1.0})
    return payload


def platform_hs_mesh() -> dict[str, Any]:
    return {
        "clientJobId": str(uuid4()),
        "centerRe": -0.75,
        "centerIm": 0.0,
        "scale": 3.0,
        "resolution": 8,
        "iterations": 16,
        "variant": "mandelbrot",
        "heightScale": 0.5,
        "heightClamp": 2.0,
        "bailout": 2.0,
    }


def platform_transition_mesh() -> dict[str, Any]:
    return {
        "clientJobId": str(uuid4()),
        "centerX": 0.0,
        "centerY": 0.0,
        "centerZ": 0.0,
        "extent": 2.0,
        "resolution": 8,
        "iterations": 16,
        "transitionFrom": "mandelbrot",
        "transitionTo": "burning_ship",
        "bailout": 2.0,
        "iso": 0.5,
        "engine": "cpu",
        "scalarType": "fp32",
    }
