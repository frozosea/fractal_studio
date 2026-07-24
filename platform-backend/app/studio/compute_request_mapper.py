"""Pure mapping from Platform recipe/output DTOs to private Compute v1 envelopes."""

from __future__ import annotations

from uuid import UUID


COMPUTE_SCHEMA_VERSION = 1
PREVIEW_MAPPING_VERSION = "compute-v1-preview-v1"
RENDER_MAPPING_VERSION = "compute-v1-render-v1"
COMPUTE_RUNS_ROUTE = "/compute/v1/runs"
COMPUTE_PREVIEWS_ROUTE = "/compute/v1/previews"


def _compute_engine(value: object) -> str:
    """Map public renderer preference to Compute v1 capability names."""
    return {"auto": "auto", "cpu": "openmp", "cuda": "cuda"}.get(str(value), "auto")


def _compute_scalar(value: object) -> str:
    return {
        "auto": "auto",
        "float": "fp32",
        "double": "fp64",
        "long_double": "fp80",
    }.get(str(value), "auto")


def _map_2d(canonical_spec: dict[str, object], *, width: int, height: int) -> dict[str, object]:
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
        "engine": _compute_engine(canonical_spec["engine"]),
        "scalarType": _compute_scalar(canonical_spec["scalarType"]),
    }
    for optional in ("juliaRe", "juliaIm"):
        if optional in canonical_spec:
            result[optional] = canonical_spec[optional]
    return result


def _envelope(*, kind: str, payload: dict[str, object], idempotency_key: str | None = None) -> dict[str, object]:
    request: dict[str, object] = {
        "schemaVersion": COMPUTE_SCHEMA_VERSION,
        "kind": kind,
        "payload": payload,
    }
    if idempotency_key is not None:
        request["idempotencyKey"] = idempotency_key
    return request


def map_preview_v1(
    canonical_spec: dict[str, object], *, width: int, height: int, request_id: UUID
) -> dict[str, object]:
    """Map bounded map preview; request_id stays Platform correlation metadata only."""
    del request_id
    return _envelope(kind="map_image", payload=_map_2d(canonical_spec, width=width, height=height))


def map_durable_v1(
    canonical_spec: dict[str, object], *, output_spec: dict[str, object], client_job_id: UUID
) -> tuple[str, dict[str, object]]:
    """Return immutable private Compute v1 request saved before worker submission."""
    kind = output_spec["kind"]
    payload: dict[str, object]
    compute_kind: str
    if kind == "image":
        compute_kind = "map_image"
        payload = _map_2d(canonical_spec, width=int(output_spec["width"]), height=int(output_spec["height"]))
    elif kind == "video":
        compute_kind = "zoom_video"
        payload = _map_2d(canonical_spec, width=int(output_spec["width"]), height=int(output_spec["height"]))
        payload.update(
            {
                "fps": output_spec["fps"],
                "durationSec": output_spec["durationSeconds"],
                # Product MVP controls duration, while Compute requires a zoom path too.
                "depthOctaves": 20.0,
            }
        )
    elif kind == "hs_mesh":
        mesh_spec = output_spec["meshSpec"]
        if not isinstance(mesh_spec, dict):
            raise ValueError("invalid_mesh_spec")
        compute_kind = "hs_mesh"
        payload = {
            "centerRe": canonical_spec["centerRe"],
            "centerIm": canonical_spec["centerIm"],
            "scale": canonical_spec["scale"],
            "resolution": output_spec["resolution"],
            "iterations": canonical_spec["iterations"],
            "variant": canonical_spec["variant"],
            "bailout": canonical_spec["bailout"],
        }
        payload.update({key: value for key, value in mesh_spec.items() if value is not None})
    elif kind == "transition_mesh":
        mesh_spec = output_spec["meshSpec"]
        if not isinstance(mesh_spec, dict):
            raise ValueError("invalid_transition_mesh_spec")
        compute_kind = "transition_mesh"
        payload = {
            "resolution": output_spec["resolution"],
            "iterations": output_spec["iterations"],
            **{key: value for key, value in mesh_spec.items() if value is not None},
        }
    else:
        raise ValueError("unsupported_output_kind")
    return (
        COMPUTE_RUNS_ROUTE,
        _envelope(
            kind=compute_kind,
            payload=payload,
            idempotency_key=f"platform-job:{client_job_id}",
        ),
    )
