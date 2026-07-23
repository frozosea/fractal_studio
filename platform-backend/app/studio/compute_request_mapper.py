"""Pure, versioned mapping from canonical recipe to private Compute DTO."""

from __future__ import annotations

from uuid import UUID


PREVIEW_MAPPING_VERSION = "map-preview-v1"
RENDER_MAPPING_VERSION = "render-v1"


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


def _map_map_fields(canonical_spec: dict[str, object], *, width: int, height: int) -> dict[str, object]:
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
    }
    for optional in ("colorMap", "juliaRe", "juliaIm"):
        if optional in canonical_spec:
            result[optional] = canonical_spec[optional]
    return result


def map_durable_v1(
    canonical_spec: dict[str, object], *, output_spec: dict[str, object], client_job_id: UUID
) -> tuple[str, dict[str, object]]:
    """Return exact private Compute route/body persisted with the job before submission."""
    kind = output_spec["kind"]
    body: dict[str, object]
    if kind == "image":
        body = _map_map_fields(canonical_spec, width=int(output_spec["width"]), height=int(output_spec["height"]))
        body.update({"clientJobId": str(client_job_id), "stillExport": True, "background": True})
        return "/api/map/render", body
    if kind == "video":
        body = _map_map_fields(canonical_spec, width=int(output_spec["width"]), height=int(output_spec["height"]))
        body.update(
            {
                "clientJobId": str(client_job_id),
                "fps": output_spec["fps"],
                "durationSeconds": output_spec["durationSeconds"],
            }
        )
        return "/api/video/export", body
    if kind == "hs_mesh":
        mesh_spec = output_spec["meshSpec"]
        if not isinstance(mesh_spec, dict):
            raise ValueError("invalid_mesh_spec")
        body = {
            "clientJobId": str(client_job_id),
            "centerRe": canonical_spec["centerRe"],
            "centerIm": canonical_spec["centerIm"],
            "scale": canonical_spec["scale"],
            "resolution": output_spec["resolution"],
            "iterations": canonical_spec["iterations"],
            "variant": canonical_spec["variant"],
            "bailout": canonical_spec["bailout"],
        }
        body.update({key: value for key, value in mesh_spec.items() if value is not None})
        return "/api/hs/mesh", body
    if kind == "transition_mesh":
        mesh_spec = output_spec["meshSpec"]
        if not isinstance(mesh_spec, dict):
            raise ValueError("invalid_transition_mesh_spec")
        body = {
            "clientJobId": str(client_job_id),
            "resolution": output_spec["resolution"],
            "iterations": output_spec["iterations"],
            **{key: value for key, value in mesh_spec.items() if value is not None},
        }
        return "/api/transition/mesh", body
    raise ValueError("unsupported_output_kind")
