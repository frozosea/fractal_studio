"""Public DTOs and the bounded Studio patch contract."""
from __future__ import annotations

from datetime import datetime
from typing import Literal
from uuid import UUID

from pydantic import BaseModel, ConfigDict, Field, ValidationError

from app.studio.models import FractalSpec


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
    "centerRe": (-1e9, 1e9), "centerIm": (-1e9, 1e9), "scale": (1e-15, 1e9),
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
    if isinstance(current, dict):
        patch = {key: value for key, value in patch.items() if current.get(key) != value}
    if not patch:
        return None
    if isinstance(current, dict) and current.get("version") == 1:
        merged = {**current, **patch}
        if merged.get("metric", "escape") == "escape" and patch.get("smooth") is True:
            return None
        if merged.get("metric", "escape") != "escape" and merged.get("colorMode", "direct") != "direct":
            # Compute silently degrades this combination. An AI suggestion must
            # never promise an equalized result which the renderer will ignore.
            return None
        try:
            # Re-run the same cross-field schema used by preview/render. The model
            # cannot smuggle in Julia/transition or colouring combinations which
            # pass scalar bounds but are invalid as a complete recipe.
            FractalSpec.model_validate(merged)
        except ValidationError:
            return None
    if patch.get("transitionMode") == "multi" and not bool(context.get("member")):
        return None
    reason = str(raw.get("reason", "AI 建议"))[:500]
    return {"patch": patch, "reason": reason}
