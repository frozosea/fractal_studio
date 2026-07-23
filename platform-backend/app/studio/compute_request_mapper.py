"""Pure, versioned mapping from canonical recipe to private Compute DTO."""

from __future__ import annotations

from uuid import UUID


PREVIEW_MAPPING_VERSION = "map-preview-v1"


def map_preview_v1(
    canonical_spec: dict[str, object], *, width: int, height: int, request_id: UUID
) -> dict[str, object]:
    """Map stable recipe fields only. No DB, time, I/O or caller-owned mutable input."""
    if canonical_spec.get("version") != 1:
        raise ValueError("unsupported_recipe_version")
    result: dict[str, object] = {
        "width": width,
        "height": height,
        "iterations": canonical_spec["iterations"],
        "variant": canonical_spec["variant"],
        "centerRe": canonical_spec["centerRe"],
        "centerIm": canonical_spec["centerIm"],
        "scale": canonical_spec["scale"],
        "julia": canonical_spec["julia"],
        "bailout": canonical_spec["bailout"],
        "engine": canonical_spec["engine"],
        "scalarType": canonical_spec["scalarType"],
        "requestId": str(request_id),
    }
    for optional in ("colorMap", "juliaRe", "juliaIm"):
        if optional in canonical_spec:
            result[optional] = canonical_spec[optional]
    return result
