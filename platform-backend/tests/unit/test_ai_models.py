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


def test_enabled_api_requires_key_but_worker_does_not_receive_it() -> None:
    base = {"database_url": "postgresql+asyncpg://unused/unused", "session_secret": "dev-secret"}
    with pytest.raises(ValidationError, match="SILICONFLOW_API_KEY"):
        Settings(**base, ai_enabled=True, ai_runtime_role="api", siliconflow_api_key="")
    worker = Settings(**base, ai_enabled=True, ai_runtime_role="worker", siliconflow_api_key="")
    assert worker.ai_enabled is True
    assert worker.siliconflow_api_key == ""
