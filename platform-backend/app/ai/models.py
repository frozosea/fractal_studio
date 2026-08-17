"""Public DTOs and the bounded Studio patch contract."""
from __future__ import annotations

import math
from datetime import datetime
from typing import Literal
from uuid import UUID

from pydantic import BaseModel, ConfigDict, Field, TypeAdapter, ValidationError, model_validator

from app.studio.models import FractalSpec, OrbitProgram
from app.studio.recipe_service import canonicalize_spec

_orbit_program_adapter = TypeAdapter(OrbitProgram)


StudioMode = Literal[
    "map", "julia", "transitionPair", "transitionMulti", "formula", "sequence"
]
_STUDIO_MODES = {
    "map", "julia", "transitionPair", "transitionMulti", "formula", "sequence",
}


def studio_mode_for_spec(spec: FractalSpec) -> StudioMode:
    if spec.julia:
        return "julia"
    if spec.transition_mode == "pair":
        return "transitionPair"
    if spec.transition_mode == "multi":
        return "transitionMulti"
    if spec.orbit_program is not None:
        return spec.orbit_program.type
    return "map"


class StudioAIOutputContext(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)

    width: int = Field(ge=64, le=4096)
    height: int = Field(ge=64, le=4096)
    preset: str | None = Field(default=None, min_length=1, max_length=64)


class StudioAIClientContext(BaseModel):
    """The complete and only browser-controlled AI context."""

    model_config = ConfigDict(extra="forbid")

    spec: FractalSpec
    mode: StudioMode
    output: StudioAIOutputContext
    # Accepted for compatibility with the current Studio payload, but discarded
    # at the HTTP boundary and replaced with the server's live Compute projection.
    capabilities: dict[str, object] | None = None

    @model_validator(mode="after")
    def mode_matches_spec(self) -> "StudioAIClientContext":
        if self.mode != studio_mode_for_spec(self.spec):
            raise ValueError("Studio mode does not match spec")
        return self


class ConversationCreate(BaseModel):
    title: str = Field(default="新对话", min_length=1, max_length=120)


class ConversationUpdate(BaseModel):
    title: str | None = Field(default=None, min_length=1, max_length=120)
    optimization_consent: bool | None = Field(default=None, alias="optimizationConsent")
    model_config = ConfigDict(populate_by_name=True)


class FeedbackInput(BaseModel):
    rating: Literal[-1, 1]
    consent: bool = False


class ConversationView(BaseModel):
    id: UUID
    title: str
    optimization_consent: bool = Field(alias="optimizationConsent")
    created_at: datetime = Field(alias="createdAt")
    updated_at: datetime = Field(alias="updatedAt")
    model_config = ConfigDict(populate_by_name=True)


class MessageView(BaseModel):
    id: UUID
    role: Literal["user", "assistant"]
    content: str
    suggestion: dict[str, object] | None = None
    created_at: datetime = Field(alias="createdAt")
    model_config = ConfigDict(populate_by_name=True)


_BOUNDED_NUMBERS: dict[str, tuple[float, float]] = {
    "centerRe": (-1e9, 1e9), "centerIm": (-1e9, 1e9),
    "scale": (3 / (2**41), 1e9),
    "iterations": (1, 1_000_000), "cyclesPerOctave": (1e-9, 64),
    "rotationDeg": (-360, 360), "pairwiseCap": (1, 1_000_000),
    "juliaRe": (-1e9, 1e9), "juliaIm": (-1e9, 1e9), "bailout": (1e-9, 1e9),
    "transitionThetaMilliDeg": (-180_000, 180_000),
}
_BOOLS = {"smooth", "julia"}
_ENUM_CAPABILITIES = {
    "variant": "variants", "colorMap": "colorMaps", "metric": "metrics",
    "engine": "engines", "scalarType": "scalars", "colorMode": "colorModes",
}
_ENUMS = {
    "transitionMode": {"off", "pair", "multi"},
}


def studio_spec_supported(spec: FractalSpec, context: dict[str, object]) -> bool:
    """Apply the complete schema, membership and live Compute contract."""

    # Use the same canonical form as recipes/previews before evaluating or
    # returning an AI-authored change. This also applies schema normalisation
    # such as escape-mode smoothing.
    try:
        spec = FractalSpec.model_validate(canonicalize_spec(spec).spec)
    except ValidationError:
        return False
    capabilities = context.get("capabilities")
    if not isinstance(capabilities, dict) or not capabilities:
        return False
    image_kinds = capabilities.get("imageKinds")
    if not isinstance(image_kinds, dict):
        return False
    kind_key = "transition" if spec.transition_mode != "off" else "map"
    kind = image_kinds.get(kind_key)
    if not isinstance(kind, dict) or kind.get("enabled") is not True:
        return False
    kind_name = "transition_image" if kind_key == "transition" else "map_image"
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
    if spec.metric != "escape" and (
        spec.color_mode != "direct" or spec.orbit_program is not None
    ):
        return False
    if spec.orbit_program is not None:
        if kind.get("orbitProgram") is not True or not bool(context.get("member")):
            return False
        programs = capabilities.get("orbitPrograms")
        if not isinstance(programs, dict) or programs.get(spec.orbit_program.type) is not True:
            return False
    if spec.transition_mode != "off":
        transition_variants = capabilities.get("axisTransitionVariants")
        if not isinstance(transition_variants, list):
            return False
        referenced = {spec.transition_from, spec.transition_to}
        referenced.update(leg.variant for leg in spec.transition_legs)
        if not referenced <= set(transition_variants):
            return False
    if spec.color_program is not None:
        gradient = capabilities.get("customGradient")
        if not isinstance(gradient, dict) or gradient.get("enabled") is not True:
            return False
        kinds = gradient.get("kinds")
        max_stops = gradient.get("maxStops")
        if not isinstance(kinds, list) or kind_name not in kinds:
            return False
        if (
            isinstance(max_stops, bool)
            or not isinstance(max_stops, int)
            or len(spec.color_program.stops) > max_stops
        ):
            return False
    return True


def studio_context_identity(
    context: dict[str, object],
) -> tuple[str, dict[str, int]] | None:
    """Return only the bounded Studio identity safe to persist with a suggestion."""
    mode = context.get("mode")
    output = context.get("output")
    if mode not in _STUDIO_MODES or not isinstance(output, dict):
        return None
    raw_spec = context.get("spec")
    if not isinstance(raw_spec, dict):
        return None
    try:
        spec = FractalSpec.model_validate(raw_spec)
    except ValidationError:
        return None
    derived_mode = studio_mode_for_spec(spec)
    if mode != derived_mode:
        return None
    dimensions: dict[str, int] = {}
    for field in ("width", "height"):
        raw = output.get(field)
        if isinstance(raw, bool) or not isinstance(raw, (int, float)):
            return None
        numeric = float(raw)
        if not math.isfinite(numeric) or not numeric.is_integer() or not 64 <= numeric <= 4096:
            return None
        dimensions[field] = int(numeric)
    return str(mode), dimensions


def validate_studio_suggestion(raw: object, context: dict[str, object]) -> dict[str, object] | None:
    """Fail closed: retain only bounded, capability-supported scalar changes."""
    if not isinstance(raw, dict):
        return None
    candidate = raw.get("patch")
    if not isinstance(candidate, dict) or not candidate or len(candidate) > 16:
        return None
    capabilities = context.get("capabilities") if isinstance(context.get("capabilities"), dict) else {}
    patch: dict[str, object] = {}
    current = context.get("spec")
    if not isinstance(current, dict):
        return None
    try:
        base_spec = FractalSpec.model_validate(current)
    except ValidationError:
        return None
    if not studio_spec_supported(base_spec, context):
        return None
    base_values = base_spec.model_dump(mode="json", by_alias=True, exclude_none=False)
    for key, value in candidate.items():
        if key in _BOUNDED_NUMBERS and isinstance(value, (int, float)) and not isinstance(value, bool):
            low, high = _BOUNDED_NUMBERS[key]
            if low <= float(value) <= high:
                patch[key] = int(value) if key in {"iterations", "pairwiseCap", "transitionThetaMilliDeg"} else float(value)
        elif key in _BOOLS and isinstance(value, bool):
            patch[key] = value
        elif key in _ENUM_CAPABILITIES and isinstance(value, str):
            allowed = capabilities.get(_ENUM_CAPABILITIES[key], []) if isinstance(capabilities, dict) else []
            if value in allowed:
                patch[key] = value
        elif key in _ENUMS and value in _ENUMS[key]:
            patch[key] = value
        elif key == "orbitProgram" and isinstance(value, dict):
            try:
                program = _orbit_program_adapter.validate_python(value)
            except ValidationError:
                pass
            else:
                patch[key] = program.model_dump(mode="json", by_alias=True)
    patch = {key: value for key, value in patch.items() if base_values.get(key) != value}
    if not patch:
        return None
    try:
        candidate_spec = FractalSpec.model_validate(base_values | patch)
    except ValidationError:
        return None
    if not studio_spec_supported(candidate_spec, context):
        return None
    canonical_base = canonicalize_spec(base_spec).spec
    canonical_candidate = canonicalize_spec(candidate_spec).spec
    patch = {
        key: canonical_candidate[key]
        for key in patch
        if key in canonical_candidate and canonical_candidate.get(key) != canonical_base.get(key)
    }
    if not patch:
        return None
    reason = raw.get("reason")
    if not isinstance(reason, str) or not reason.strip():
        return None
    reason = reason.strip()[:500]
    identity = studio_context_identity(context)
    if identity is None:
        return None
    base_mode, base_output = identity
    return {
        "patch": patch,
        "reason": reason,
        "baseSpec": base_spec.model_dump(
            mode="json", by_alias=True, exclude_none=False, exclude_unset=True
        ),
        "baseMode": base_mode,
        "baseOutput": base_output,
    }
