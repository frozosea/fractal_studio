"""Studio AI patch and configuration safety contracts."""
from __future__ import annotations

import pytest
from pydantic import ValidationError

from app.ai.models import validate_studio_suggestion
from app.core.config import Settings


def _context(*, member: bool = False) -> dict[str, object]:
    return {
        "member": member,
        "spec": {"version": 1, "variant": "mandelbrot", "metric": "escape"},
        "capabilities": {
            "variants": ["mandelbrot", "tricorn"],
            "colorMaps": ["inferno", "viridis"],
            "metrics": ["escape", "min_abs"],
            "colorModes": ["direct", "eq_full"],
            "engines": ["auto", "cuda"],
            "scalars": ["auto", "fp64"],
        },
    }


def _context_with_image_kinds(*, member: bool = False) -> dict[str, object]:
    context = _context(member=member)
    context["capabilities"]["imageKinds"] = {
        "map": {
            "enabled": True,
            "metrics": ["escape"],
            "engines": ["auto"],
            "scalars": ["auto"],
        },
        "transition": {
            "enabled": True,
            "metrics": ["escape", "min_abs"],
            "engines": ["cuda"],
            "scalars": ["fp64"],
        },
    }
    return context


def test_patch_keeps_only_bounded_capability_supported_fields() -> None:
    result = validate_studio_suggestion(
        {
            "patch": {
                "colorMap": "viridis",
                "iterations": 4096,
                "engine": "cuda",
                "unknown": "drop me",
                "rotationDeg": 999,
            },
            "reason": "冷色且保留结构",
        },
        _context(),
    )
    assert result == {
        "patch": {"colorMap": "viridis", "iterations": 4096, "engine": "cuda"},
        "reason": "冷色且保留结构",
    }


@pytest.mark.parametrize(
    "patch",
    [
        {"colorMap": "not-a-capability"},
        {"iterations": 0},
        {"scale": 1e-14},
        {"julia": True},  # complete FractalSpec requires both Julia constants
        {"metric": "min_abs", "colorMode": "eq_full"},
    ],
)
def test_invalid_or_incompatible_patch_fails_closed(patch: dict[str, object]) -> None:
    assert validate_studio_suggestion({"patch": patch, "reason": "bad"}, _context()) is None


def test_member_only_multi_transition_is_rejected_for_trial_user() -> None:
    proposal = {"patch": {"transitionMode": "multi"}, "reason": "多轴过渡"}
    assert validate_studio_suggestion(proposal, _context(member=False)) is None
    assert validate_studio_suggestion(proposal, _context(member=True)) == proposal


@pytest.mark.parametrize("reason", [None, "", {"not": "text"}, ["not", "text"]])
def test_patch_requires_a_real_text_reason(reason: object) -> None:
    assert validate_studio_suggestion(
        {"patch": {"colorMap": "viridis"}, "reason": reason},
        _context(),
    ) is None


@pytest.mark.parametrize(
    "patch",
    [
        {"metric": "min_abs"},
        {"engine": "cuda"},
        {"scalarType": "fp64"},
    ],
)
def test_map_patch_rejects_values_only_supported_by_transition_kind(
    patch: dict[str, object],
) -> None:
    assert validate_studio_suggestion(
        {"patch": patch, "reason": "wrong kind"},
        _context_with_image_kinds(),
    ) is None


def test_transition_patch_uses_transition_kind_capabilities_after_merge() -> None:
    proposal = {
        "patch": {
            "transitionMode": "pair",
            "metric": "min_abs",
            "engine": "cuda",
            "scalarType": "fp64",
        },
        "reason": "transition-compatible",
    }
    assert validate_studio_suggestion(
        proposal,
        _context_with_image_kinds(),
    ) == proposal


def test_existing_transition_mode_selects_transition_kind_for_follow_up_patch() -> None:
    context = _context_with_image_kinds()
    context["spec"]["transitionMode"] = "pair"
    assert validate_studio_suggestion(
        {"patch": {"engine": "auto"}, "reason": "map-only engine"},
        context,
    ) is None


def test_transition_mode_change_rejects_merged_map_only_engine() -> None:
    context = _context_with_image_kinds()
    context["spec"]["engine"] = "auto"
    assert validate_studio_suggestion(
        {"patch": {"transitionMode": "pair"}, "reason": "engine would be unsupported"},
        context,
    ) is None


def test_disabled_transition_kind_rejects_transition_patch() -> None:
    context = _context_with_image_kinds(member=True)
    context["capabilities"]["imageKinds"]["transition"]["enabled"] = False
    assert validate_studio_suggestion(
        {"patch": {"transitionMode": "multi"}, "reason": "unavailable kind"},
        context,
    ) is None


def test_enabled_api_requires_key_but_worker_does_not_receive_it() -> None:
    base = {"database_url": "postgresql+asyncpg://unused/unused", "session_secret": "dev-secret"}
    with pytest.raises(ValidationError, match="SILICONFLOW_API_KEY"):
        Settings(**base, ai_enabled=True, ai_runtime_role="api", siliconflow_api_key="")
    worker = Settings(**base, ai_enabled=True, ai_runtime_role="worker", siliconflow_api_key="")
    assert worker.ai_enabled is True
    assert worker.siliconflow_api_key == ""
