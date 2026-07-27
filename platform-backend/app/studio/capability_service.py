"""Small cached projection of private Compute capabilities for Studio UI."""

from __future__ import annotations

import time

from app.infrastructure.compute.compute_client import ComputeClient, ComputeClientError

_TTL_SECONDS = 60.0
_cached_at = 0.0
_cached: dict[str, object] | None = None


async def studio_capabilities() -> dict[str, object]:
    global _cached_at, _cached
    if _cached is not None and time.monotonic() - _cached_at < _TTL_SECONDS:
        return _cached

    raw = await ComputeClient().capabilities()
    jobs = raw.get("jobs")
    job_items = {
        str(item.get("kind")): item
        for item in jobs
        if isinstance(item, dict) and isinstance(item.get("kind"), str)
    } if isinstance(jobs, list) else {}
    map_job = job_items.get("map_image")
    transition_job = job_items.get("transition_image")
    coloring = raw.get("coloring") if isinstance(raw.get("coloring"), dict) else {}
    if not isinstance(map_job, dict):
        raise ComputeClientError("compute_invalid_capabilities")

    def strings(value: object) -> list[str]:
        return [item for item in value if isinstance(item, str)] if isinstance(value, list) else []

    _cached = {
        "rendererVersion": raw.get("rendererVersion"),
        "metrics": strings(map_job.get("metrics")),
        "engines": strings(map_job.get("engines")),
        "scalars": strings(map_job.get("scalars")),
        "colorMaps": strings(coloring.get("builtInColorMaps")),
        "colorModes": strings(coloring.get("staticImageColorModes")),
        "variants": strings(raw.get("builtInVariants")),
        "axisTransitionVariants": strings(raw.get("axisTransitionVariants")),
        "imageKinds": {
            "map": {
                "enabled": True,
                "metrics": strings(map_job.get("metrics")),
                "engines": strings(map_job.get("engines")),
                "scalars": strings(map_job.get("scalars")),
                "orbitProgram": map_job.get("orbitProgram") is True,
            },
            "transition": {
                "enabled": isinstance(transition_job, dict),
                "metrics": strings(transition_job.get("metrics")) if isinstance(transition_job, dict) else [],
                "engines": strings(transition_job.get("engines")) if isinstance(transition_job, dict) else [],
                "scalars": strings(transition_job.get("scalars")) if isinstance(transition_job, dict) else [],
                "orbitProgram": False,
            },
        },
        "orbitPrograms": raw.get("orbitPrograms") if isinstance(raw.get("orbitPrograms"), dict) else {},
        "customGradient": {
            "enabled": coloring.get("customGradient") is True,
            "maxStops": coloring.get("customGradientMaxStops") if isinstance(coloring.get("customGradientMaxStops"), int) else 0,
            "kinds": strings(coloring.get("customGradientKinds")),
        },
    }
    _cached_at = time.monotonic()
    return _cached
