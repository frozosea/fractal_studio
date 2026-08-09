"""Validated multi-candidate contracts for Studio's three exploration assistants."""
from __future__ import annotations

import json
import math
from decimal import Decimal, localcontext
from typing import Literal

from pydantic import ValidationError

from app.studio.models import ColorProgram, FractalSpec
from app.studio.recipe_service import canonicalize_spec

ExplorationMode = Literal["location", "color", "composition"]

_COUNTS: dict[ExplorationMode, int] = {
    "location": 3,
    "color": 4,
    "composition": 3,
}
_COLOR_FIELDS = {
    "metric", "smooth", "colorMode", "colorMap", "colorProgram",
    "cyclesPerOctave", "bailout",
}
_MIN_STUDIO_SCALE = Decimal(3) / (Decimal(2) ** 41)


def _finite_number(value: object) -> float | None:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    try:
        number = float(value)
    except (OverflowError, TypeError, ValueError):
        return None
    return number if math.isfinite(number) else None


def _decimal_text(value: Decimal) -> str:
    text = format(value, "f")
    if "." in text:
        text = text.rstrip("0").rstrip(".")
    return text if text not in {"", "-0"} else "0"


def _base_spec(context: dict[str, object]) -> FractalSpec | None:
    raw = context.get("spec")
    if not isinstance(raw, dict):
        return None
    try:
        return FractalSpec.model_validate(raw)
    except ValidationError:
        return None


def _kind_capabilities(
    spec: FractalSpec, capabilities: dict[str, object]
) -> tuple[str, dict[str, object]] | None:
    image_kinds = capabilities.get("imageKinds")
    if not isinstance(image_kinds, dict):
        return None
    key = "transition" if spec.transition_mode != "off" else "map"
    kind = image_kinds.get(key)
    if not isinstance(kind, dict) or kind.get("enabled") is not True:
        return None
    return ("transition_image" if key == "transition" else "map_image", kind)


def _supported(spec: FractalSpec, context: dict[str, object]) -> bool:
    capabilities = context.get("capabilities")
    if not isinstance(capabilities, dict):
        return False
    resolved = _kind_capabilities(spec, capabilities)
    if resolved is None:
        return False
    kind_name, kind = resolved
    for value, key in (
        (spec.metric, "metrics"),
        (spec.engine, "engines"),
        (spec.scalar_type, "scalars"),
    ):
        allowed = kind.get(key)
        if not isinstance(allowed, list) or value not in allowed:
            return False
    variants = capabilities.get("variants")
    if not isinstance(variants, list) or spec.variant not in variants:
        return False
    if spec.color_map is not None:
        color_maps = capabilities.get("colorMaps")
        if not isinstance(color_maps, list) or spec.color_map not in color_maps:
            return False
    color_modes = capabilities.get("colorModes")
    if not isinstance(color_modes, list) or spec.color_mode not in color_modes:
        return False
    if spec.transition_mode == "multi" and not bool(context.get("member")):
        return False
    if spec.metric != "escape":
        if spec.color_mode != "direct" or spec.orbit_program is not None:
            return False
    if spec.orbit_program is not None:
        if kind.get("orbitProgram") is not True or not bool(context.get("member")):
            return False
        programs = capabilities.get("orbitPrograms")
        if not isinstance(programs, dict) or programs.get(spec.orbit_program.type) is not True:
            return False
    if spec.transition_mode != "off":
        variants = capabilities.get("axisTransitionVariants")
        if not isinstance(variants, list):
            return False
        referenced = {spec.transition_from, spec.transition_to}
        referenced.update(leg.variant for leg in spec.transition_legs)
        if not referenced <= set(variants):
            return False
    if spec.color_program is not None:
        gradient = capabilities.get("customGradient")
        if not isinstance(gradient, dict) or gradient.get("enabled") is not True:
            return False
        kinds = gradient.get("kinds")
        max_stops = gradient.get("maxStops")
        if not isinstance(kinds, list) or kind_name not in kinds:
            return False
        if not isinstance(max_stops, int) or len(spec.color_program.stops) > max_stops:
            return False
    return True


def _output_aspect(context: dict[str, object]) -> Decimal | None:
    output = context.get("output")
    if not isinstance(output, dict):
        return None
    width = _finite_number(output.get("width"))
    height = _finite_number(output.get("height"))
    if width is None or height is None or width < 64 or height < 64:
        return None
    # Navigation keeps the exact decimal center strings authoritative.  Do not
    # let the process-wide Decimal precision silently round an aspect ratio
    # before it is applied to a deep-zoom center.
    with localcontext() as decimal_context:
        decimal_context.prec = 96
        return Decimal(str(width)) / Decimal(str(height))


def _navigation_patch(
    base: FractalSpec,
    *,
    offset_x: float,
    offset_y: float,
    scale_factor: float,
    aspect: Decimal,
    rotation_delta: float = 0,
) -> dict[str, object] | None:
    center_re = Decimal(base.center_re_str or str(base.center_re))
    center_im = Decimal(base.center_im_str or str(base.center_im))
    scale = Decimal(str(base.scale))
    factor = Decimal(str(scale_factor))
    next_scale = scale * factor
    if next_scale < _MIN_STUDIO_SCALE or next_scale > Decimal("1000000000"):
        return None
    angle = math.radians(base.rotation_deg)
    cos_value = Decimal(str(math.cos(angle)))
    sin_value = Decimal(str(math.sin(angle)))
    with localcontext() as decimal_context:
        decimal_context.prec = 96
        local_re = Decimal(str(offset_x)) * scale * aspect
        local_im = Decimal(str(offset_y)) * scale
        next_re = center_re + local_re * cos_value - local_im * sin_value
        next_im = center_im + local_re * sin_value + local_im * cos_value
    patch: dict[str, object] = {
        "centerRe": float(next_re),
        "centerIm": float(next_im),
        "centerReStr": _decimal_text(next_re),
        "centerImStr": _decimal_text(next_im),
        "scale": float(next_scale),
    }
    if rotation_delta:
        patch["rotationDeg"] = ((base.rotation_deg + rotation_delta + 180) % 360) - 180
    current = base.model_dump(mode="json", by_alias=True, exclude_none=True)
    return {key: value for key, value in patch.items() if current.get(key) != value}


def _label_reason(raw: dict[str, object]) -> tuple[str, str] | None:
    raw_label = raw.get("label")
    raw_reason = raw.get("reason")
    if not isinstance(raw_label, str) or not isinstance(raw_reason, str):
        return None
    label = raw_label.strip()
    reason = raw_reason.strip()
    if not label or not reason:
        return None
    return label[:60], reason[:500]


def _candidate_result(
    *, mode: ExplorationMode, base: FractalSpec, candidates: list[dict[str, object]]
) -> dict[str, object]:
    canonical = canonicalize_spec(base)
    return {
        "kind": "candidate_set",
        "mode": mode,
        "baseSpecHash": canonical.sha256,
        # The browser compares this validated request snapshot with its current
        # Studio state before preview/apply. Recipe SHA formatting intentionally
        # stays server-only because Python/orjson and JavaScript number spelling
        # are not a portable canonicalisation contract.
        "baseSpec": base.model_dump(
            mode="json", by_alias=True, exclude_none=False, exclude_unset=True
        ),
        "candidates": [
            {
                "id": chr(ord("A") + index),
                **candidate,
                "verification": "pending",
            }
            for index, candidate in enumerate(candidates)
        ],
    }


def _navigation_candidates(
    raw: dict[str, object], context: dict[str, object], mode: Literal["location", "composition"]
) -> dict[str, object] | None:
    base = _base_spec(context)
    aspect = _output_aspect(context)
    items = raw.get("candidates")
    if base is None or aspect is None or not _supported(base, context) or not isinstance(items, list):
        return None
    if len(items) != _COUNTS[mode]:
        return None
    axis = raw.get("axis") if mode == "location" else None
    if mode == "location" and axis not in {"position", "scale"}:
        return None
    candidates: list[dict[str, object]] = []
    fingerprints: set[str] = set()
    semantic_vectors: list[tuple[float, ...]] = []
    for item in items:
        if not isinstance(item, dict) or (text := _label_reason(item)) is None:
            return None
        offset_x = _finite_number(item.get("offsetX"))
        offset_y = _finite_number(item.get("offsetY"))
        scale_factor = _finite_number(item.get("scaleFactor"))
        rotation_delta = _finite_number(item.get("rotationDelta", 0))
        if None in {offset_x, offset_y, scale_factor, rotation_delta}:
            return None
        assert offset_x is not None and offset_y is not None
        assert scale_factor is not None and rotation_delta is not None
        if mode == "location":
            if abs(offset_x) > 0.45 or abs(offset_y) > 0.45 or not 0.45 <= scale_factor <= 1.8:
                return None
            if axis == "position" and (scale_factor != 1 or (offset_x == 0 and offset_y == 0)):
                return None
            if axis == "scale" and (offset_x != 0 or offset_y != 0 or scale_factor == 1):
                return None
            rotation_delta = 0
        else:
            if (
                abs(offset_x) > 0.25 or abs(offset_y) > 0.25
                or not 0.75 <= scale_factor <= 1.33 or abs(rotation_delta) > 30
                or (offset_x == 0 and offset_y == 0 and scale_factor == 1 and rotation_delta == 0)
            ):
                return None
        if mode == "location" and axis == "position":
            vector = (offset_x, offset_y)
            if math.hypot(*vector) < 0.04:
                return None
        elif mode == "location":
            vector = (math.log(scale_factor),)
            if abs(vector[0]) < math.log(1.04):
                return None
        else:
            vector = (
                offset_x / 0.25,
                offset_y / 0.25,
                math.log(scale_factor) / math.log(1.33),
                rotation_delta / 30,
            )
            if math.sqrt(sum(value * value for value in vector)) < 0.12:
                return None
        minimum_distance = 0.04 if mode == "location" else 0.15
        if any(
            math.sqrt(sum((left - right) ** 2 for left, right in zip(vector, prior)))
            < minimum_distance
            for prior in semantic_vectors
        ):
            return None
        semantic_vectors.append(vector)
        patch = _navigation_patch(
            base,
            offset_x=offset_x,
            offset_y=offset_y,
            scale_factor=scale_factor,
            aspect=aspect,
            rotation_delta=rotation_delta,
        )
        if not patch:
            return None
        merged = base.model_dump(mode="json", by_alias=True, exclude_none=False) | patch
        try:
            candidate_spec = FractalSpec.model_validate(merged)
        except ValidationError:
            return None
        if not _supported(candidate_spec, context):
            return None
        fingerprint = json.dumps(patch, sort_keys=True, separators=(",", ":"))
        if fingerprint in fingerprints:
            return None
        fingerprints.add(fingerprint)
        label, reason = text
        candidates.append({"label": label, "patch": patch, "reason": reason})
    return _candidate_result(mode=mode, base=base, candidates=candidates)


def _color_candidates(raw: dict[str, object], context: dict[str, object]) -> dict[str, object] | None:
    base = _base_spec(context)
    items = raw.get("candidates")
    if base is None or not _supported(base, context) or not isinstance(items, list):
        return None
    if len(items) != _COUNTS["color"]:
        return None
    base_values = base.model_dump(mode="json", by_alias=True, exclude_none=False)
    candidates: list[dict[str, object]] = []
    fingerprints: set[str] = set()
    for item in items:
        if not isinstance(item, dict) or (text := _label_reason(item)) is None:
            return None
        raw_patch = item.get("patch")
        if not isinstance(raw_patch, dict) or not raw_patch or not set(raw_patch) <= _COLOR_FIELDS:
            return None
        if raw_patch.get("colorMap") is None and "colorMap" in raw_patch and "colorProgram" not in raw_patch:
            return None
        patch = dict(raw_patch)
        if "colorProgram" in patch:
            if patch["colorProgram"] is None:
                return None
            try:
                program = ColorProgram.model_validate(patch["colorProgram"])
            except ValidationError:
                return None
            if len(program.stops) > 6:
                return None
            period = 1.0 if program.wrap == "repeat" else 2.0 if program.wrap == "mirror" else None
            normalized_phase = (
                float(Decimal(str(program.phase)) % Decimal(str(period)))
                if period is not None
                else program.phase
            )
            normalized_program = program.model_dump(mode="json", by_alias=True)
            normalized_program["phase"] = normalized_phase
            normalized_program["interiorColor"] = program.interior_color.lower()
            normalized_program["invalidColor"] = program.invalid_color.lower()
            normalized_program["stops"] = [
                {"at": stop.at, "color": stop.color.lower()} for stop in program.stops
            ]
            patch["colorProgram"] = normalized_program
            patch["colorMap"] = None
            patch["colorMode"] = "direct"
        elif isinstance(patch.get("colorMap"), str):
            patch["colorProgram"] = None
        merged = base_values | patch
        try:
            candidate_spec = FractalSpec.model_validate(merged)
        except ValidationError:
            return None
        if not _supported(candidate_spec, context):
            return None
        normalized = candidate_spec.model_dump(mode="json", by_alias=True, exclude_none=False)
        effective_patch = {
            key: normalized[key]
            for key in _COLOR_FIELDS
            if normalized.get(key) != base_values.get(key)
        }
        if not effective_patch:
            return None
        fingerprint = json.dumps(effective_patch, sort_keys=True, separators=(",", ":"))
        if fingerprint in fingerprints:
            return None
        fingerprints.add(fingerprint)
        label, reason = text
        candidates.append({"label": label, "patch": effective_patch, "reason": reason})
    return _candidate_result(mode="color", base=base, candidates=candidates)


def validate_candidate_set(
    raw: object, context: dict[str, object], mode: ExplorationMode
) -> dict[str, object] | None:
    """Fail closed and return a server-derived, replay-safe candidate set."""
    if not isinstance(raw, dict):
        return None
    if mode == "color":
        return _color_candidates(raw, context)
    return _navigation_candidates(raw, context, mode)
