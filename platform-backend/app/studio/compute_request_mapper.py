"""Pure mapping from Platform recipe/output DTOs to private Compute v1 envelopes."""

from __future__ import annotations

import math
from copy import deepcopy
from uuid import UUID


COMPUTE_SCHEMA_VERSION = 1
PREVIEW_MAPPING_VERSION = "compute-v1-preview-v3"
RENDER_MAPPING_VERSION = "compute-v1-render-v1"
# Preview is interactive work, not an export. High iteration counts can
# occupy a Compute slot for a while (especially exp/sin maps at deep zoom),
# so the cap is a safety ceiling rather than a detail target: viewport
# iterations up to this value render at their true count, larger requests
# are clamped here and the escape-gradient correction keeps colour bands
# aligned with the durable render.
PREVIEW_MAX_ITERATIONS = 65536
PREVIEW_MAX_PAIRWISE_CAP = 128
# Durable renders are billed exports, but the shared node capacity still needs
# a hard ceiling: these mirror the Studio UI bounds and the frontend AI-patch
# clamps. Out-of-range values are rejected, never silently resized.
RENDER_MAX_ITERATIONS = 1_000_000
RENDER_MIN_SCALE = 3 / 2**41
RENDER_MAX_SCALE = 1e9
RENDER_MAX_BAILOUT = 1e9
RENDER_MAX_PAIRWISE_CAP = 1_000_000
COMPUTE_RUNS_ROUTE = "/compute/v1/runs"
COMPUTE_PREVIEWS_ROUTE = "/compute/v1/previews"
# Mapper failures safe to echo back to the caller as a 422 detail.
PUBLIC_MAPPING_ERRORS = frozenset(
    {
        "unsupported_recipe_version",
        "unsupported_output_kind",
        "incomplete_canonical_spec",
        "color_program_unsupported_for_output",
    }
)


def _compute_engine(value: object) -> str:
    """Map public renderer preference to Compute v1 capability names."""
    return {"cpu": "openmp"}.get(str(value), str(value))


def _compute_scalar(value: object) -> str:
    return {
        "auto": "auto",
        "float": "fp32",
        "double": "fp64",
        "long_double": "fp80",
    }.get(str(value), str(value))


_REQUIRED_2D_KEYS = (
    "iterations",
    "variant",
    "centerRe",
    "centerIm",
    "scale",
    "julia",
    "bailout",
    "metric",
    "smooth",
    "colorMode",
    "cyclesPerOctave",
    "rotationDeg",
    "pairwiseCap",
    "engine",
    "scalarType",
    "transitionMode",
)


def _map_2d(canonical_spec: dict[str, object], *, width: int, height: int) -> dict[str, object]:
    if canonical_spec.get("version") != 1:
        raise ValueError("unsupported_recipe_version")
    if any(key not in canonical_spec for key in _REQUIRED_2D_KEYS):
        raise ValueError("incomplete_canonical_spec")
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
        "metric": canonical_spec["metric"],
        "smooth": canonical_spec["smooth"],
        "colorMode": canonical_spec["colorMode"],
        "cyclesPerOctave": canonical_spec["cyclesPerOctave"],
        "rotationDeg": canonical_spec["rotationDeg"],
        "pairwiseCap": canonical_spec["pairwiseCap"],
        "engine": _compute_engine(canonical_spec["engine"]),
        "scalarType": _compute_scalar(canonical_spec["scalarType"]),
    }
    for optional in (
        "centerReStr", "centerImStr", "juliaRe", "juliaIm", "colorMap",
        "colorProgram", "orbitProgram",
    ):
        if optional in canonical_spec:
            result[optional] = canonical_spec[optional]
    transition_mode = canonical_spec["transitionMode"]
    if transition_mode == "pair":
        result.update(
            {
                "transitionThetaMilliDeg": canonical_spec["transitionThetaMilliDeg"],
                "transitionFrom": canonical_spec["transitionFrom"],
                "transitionTo": canonical_spec["transitionTo"],
            }
        )
    elif transition_mode == "multi":
        result.update(
            {
                "transitionThetaMilliDeg": canonical_spec["transitionThetaMilliDeg"],
                "transitionLegs": canonical_spec["transitionLegs"],
            }
        )
    return result


def _static_image_kind(canonical_spec: dict[str, object]) -> str:
    return "transition_image" if canonical_spec.get("transitionMode") in {"pair", "multi"} else "map_image"


def _envelope(*, kind: str, payload: dict[str, object], idempotency_key: str | None = None) -> dict[str, object]:
    request: dict[str, object] = {
        "schemaVersion": COMPUTE_SCHEMA_VERSION,
        "kind": kind,
        "payload": payload,
    }
    if idempotency_key is not None:
        request["idempotencyKey"] = idempotency_key
    return request


def _bounded_int(value: object, *, name: str, minimum: int, maximum: int) -> int:
    try:
        number = int(value)
    except (TypeError, ValueError) as error:
        raise ValueError(f"invalid_{name}") from error
    if number < minimum or number > maximum:
        raise ValueError(f"invalid_{name}")
    return number


def _bounded_float(value: object, *, name: str, minimum: float, maximum: float) -> float:
    try:
        number = float(value)
    except (TypeError, ValueError) as error:
        raise ValueError(f"invalid_{name}") from error
    if not math.isfinite(number) or number < minimum or number > maximum:
        raise ValueError(f"invalid_{name}")
    return number


def bound_durable_payload(payload: dict[str, object]) -> dict[str, object]:
    """Reject out-of-range work before a durable request is persisted/sent.

    The canonical recipe comes from the user's own edits, but may also have
    been produced from AI suggestions, so it is not trusted input. Enforcing
    the bounds here keeps a single account from occupying shared CPU/GPU pool
    slots with arbitrarily expensive render jobs.
    """
    bounded = deepcopy(payload)
    bounded["iterations"] = _bounded_int(
        bounded.get("iterations", 512), name="iterations", minimum=1, maximum=RENDER_MAX_ITERATIONS
    )
    if "scale" in bounded:
        bounded["scale"] = _bounded_float(
            bounded.get("scale", 3), name="scale", minimum=RENDER_MIN_SCALE, maximum=RENDER_MAX_SCALE
        )
    if "bailout" in bounded:
        bounded["bailout"] = _bounded_float(
            bounded.get("bailout", 2), name="bailout", minimum=1e-6, maximum=RENDER_MAX_BAILOUT
        )
    if "pairwiseCap" in bounded:
        bounded["pairwiseCap"] = _bounded_int(
            bounded.get("pairwiseCap", 0), name="pairwiseCap", minimum=0, maximum=RENDER_MAX_PAIRWISE_CAP
        )
    return bounded


def bound_preview_payload(payload: dict[str, object]) -> dict[str, object]:
    """Copy and bound preview work while preserving escape-gradient colour bands."""
    bounded = deepcopy(payload)
    original_iterations = max(1, int(bounded.get("iterations", 1)))
    preview_iterations = min(original_iterations, PREVIEW_MAX_ITERATIONS)
    bounded["iterations"] = preview_iterations
    bounded["pairwiseCap"] = min(
        int(bounded.get("pairwiseCap", PREVIEW_MAX_PAIRWISE_CAP)),
        PREVIEW_MAX_PAIRWISE_CAP,
    )
    if "scale" in bounded:
        try:
            bounded["scale"] = min(
                max(float(bounded["scale"]), RENDER_MIN_SCALE), RENDER_MAX_SCALE
            )
        except (TypeError, ValueError):
            bounded["scale"] = 3
    program = bounded.get("colorProgram")
    if (
        preview_iterations < original_iterations
        and bounded.get("metric") == "escape"
        and isinstance(program, dict)
        and isinstance(program.get("cycles"), (int, float))
    ):
        # Compute maps escape bands with (iter + 1) / (iterations + 2).
        # Scaling by the exact denominator ratio keeps an early-escaping
        # pixel at the same point in the colour program as the durable render.
        program["cycles"] = float(program["cycles"]) * (
            (preview_iterations + 2) / (original_iterations + 2)
        )
    return bounded


def map_preview_v1(
    canonical_spec: dict[str, object], *, width: int, height: int, request_id: UUID
) -> dict[str, object]:
    """Map bounded map preview; request_id stays Platform correlation metadata only."""
    del request_id
    payload = _map_2d(canonical_spec, width=width, height=height)
    # A preview is interactive and must not inherit export-sized work limits.
    # Keep the full recipe immutable for durable renders while bounding the two
    # parameters that can otherwise turn a small preview into minutes of work.
    payload = bound_preview_payload(payload)
    return _envelope(kind=_static_image_kind(canonical_spec), payload=payload)


def map_durable_v1(
    canonical_spec: dict[str, object], *, output_spec: dict[str, object], client_job_id: UUID
) -> tuple[str, dict[str, object]]:
    """Return immutable private Compute v1 request saved before worker submission."""
    kind = output_spec["kind"]
    payload: dict[str, object]
    compute_kind: str
    if kind == "image":
        compute_kind = _static_image_kind(canonical_spec)
        payload = _map_2d(canonical_spec, width=int(output_spec["width"]), height=int(output_spec["height"]))
    elif kind == "video":
        compute_kind = "zoom_video"
        payload = _map_2d(canonical_spec, width=int(output_spec["width"]), height=int(output_spec["height"]))
        # Custom gradients ship for 2D preview/PNG only, see docs/coloring_contract.md.
        if "colorProgram" in payload:
            raise ValueError("color_program_unsupported_for_output")
        # zoom_video uses the ln-map colour contract, while the Studio's
        # static-image controls expose the friendlier direct/eq_* names.
        # Do not forward ``colorMode``: Compute treats it as lnMapColorMode
        # when the explicit field is absent.
        color_mode = str(payload.pop("colorMode"))
        ln_map_color_mode = {"direct": "escape", "eq_full": "hist_eq", "eq_center": "row_eq"}[color_mode]
        payload.update(
            {
                "fps": output_spec["fps"],
                "durationSec": output_spec["durationSeconds"],
                # Jobs stored before depthOctaves existed keep the historical constant.
                "depthOctaves": output_spec.get("depthOctaves", 20.0),
                "lnMapColorMode": ln_map_color_mode,
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
    payload = bound_durable_payload(payload)
    return (
        COMPUTE_RUNS_ROUTE,
        _envelope(
            kind=compute_kind,
            payload=payload,
            idempotency_key=f"platform-job:{client_job_id}",
        ),
    )
