"""Studio AI patch and configuration safety contracts."""
from __future__ import annotations

import pytest
from pydantic import ValidationError

from app.ai.models import validate_studio_suggestion
from app.core.config import Settings


def _context(*, member: bool = False) -> dict[str, object]:
    return {
        "member": member,
        "mode": "map",
        "output": {"width": 1600, "height": 1200},
        "spec": {"version": 1, "variant": "mandelbrot", "metric": "escape"},
        "capabilities": {
            "variants": ["mandelbrot", "tricorn"],
            "colorMaps": ["inferno", "viridis"],
            "metrics": ["escape", "min_abs"],
            "colorModes": ["direct", "eq_full"],
            "engines": ["auto", "cuda"],
            "scalars": ["auto", "fp64"],
            "axisTransitionVariants": ["mandelbrot", "burning_ship"],
            "orbitPrograms": {"formula": True, "sequence": True},
            "imageKinds": {
                "map": {
                    "enabled": True,
                    "metrics": ["escape", "min_abs"],
                    "engines": ["auto", "cuda"],
                    "scalars": ["auto", "fp64"],
                    "orbitProgram": True,
                },
                "transition": {
                    "enabled": True,
                    "metrics": ["escape", "min_abs"],
                    "engines": ["auto", "cuda"],
                    "scalars": ["auto", "fp64"],
                    "orbitProgram": False,
                },
            },
            "customGradient": {
                "enabled": True,
                "maxStops": 16,
                "kinds": ["map_image", "transition_image"],
            },
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
        "baseSpec": {
            "version": 1, "variant": "mandelbrot", "metric": "escape", "smooth": False,
        },
        "baseMode": "map",
        "baseOutput": {"width": 1600, "height": 1200},
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
    result = validate_studio_suggestion(proposal, _context(member=True))
    assert result is not None
    assert result["patch"] == proposal["patch"]
    assert result["baseMode"] == "map"


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
    result = validate_studio_suggestion(
        proposal,
        _context_with_image_kinds(),
    )
    assert result is not None
    assert result["patch"] == proposal["patch"]
    assert result["baseOutput"] == {"width": 1600, "height": 1200}


@pytest.mark.parametrize(
    ("mode", "output"),
    [
        ("forged", {"width": 1600, "height": 1200}),
        ("julia", {"width": 1600, "height": 1200}),
        ("map", {"width": float("nan"), "height": 1200}),
        ("map", {"width": 1600.5, "height": 1200}),
        ("map", {"width": 8192, "height": 1200}),
    ],
)
def test_patch_rejects_untrusted_baseline_identity(mode: str, output: dict[str, object]) -> None:
    context = _context()
    context["mode"] = mode
    context["output"] = output
    assert validate_studio_suggestion(
        {"patch": {"colorMap": "viridis"}, "reason": "invalid baseline"},
        context,
    ) is None


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


def test_numeric_patch_rejects_empty_or_incompatible_compute_contract() -> None:
    context = _context()
    context["capabilities"] = {}
    assert validate_studio_suggestion(
        {"patch": {"iterations": 1024}, "reason": "no live Compute contract"},
        context,
    ) is None

    context = _context()
    context["spec"]["variant"] = "unsupported-on-compute"
    assert validate_studio_suggestion(
        {"patch": {"iterations": 1024}, "reason": "unsupported base"},
        context,
    ) is None


def test_formula_patch_enforces_membership_orbit_and_metric_contracts() -> None:
    context = _context()
    context["mode"] = "formula"
    context["spec"].update({
        "orbitProgram": {
            "type": "formula",
            "formula": {"type": "builtin", "id": "mandelbrot"},
        },
        "colorMode": "direct",
    })
    proposal = {"patch": {"iterations": 1024}, "reason": "formula refinement"}
    assert validate_studio_suggestion(proposal, context) is None

    context["member"] = True
    context["capabilities"]["orbitPrograms"]["formula"] = False
    assert validate_studio_suggestion(proposal, context) is None

    context["capabilities"]["orbitPrograms"]["formula"] = True
    assert validate_studio_suggestion(proposal, context) is not None
    assert validate_studio_suggestion(
        {"patch": {"metric": "min_abs"}, "reason": "invalid formula metric"},
        context,
    ) is None


def test_generic_patch_uses_canonical_effective_difference() -> None:
    result = validate_studio_suggestion(
        {
            "patch": {"smooth": True, "iterations": 1024},
            "reason": "escape smoothing is canonicalized away",
        },
        _context(),
    )
    assert result is not None
    assert result["patch"] == {"iterations": 1024}


def test_generic_patch_reuses_color_program_mutual_exclusion() -> None:
    context = _context(member=True)
    context["spec"].update({
        "colorMode": "direct",
        "colorProgram": {
            "wrap": "repeat",
            "cycles": 2,
            "phase": 0,
            "interiorColor": "#000000",
            "invalidColor": "#ff00ff",
            "stops": [
                {"at": 0, "color": "#000000"},
                {"at": 1, "color": "#ffffff"},
            ],
        },
    })

    assert validate_studio_suggestion(
        {"patch": {"colorMap": "viridis"}, "reason": "cannot keep both color sources"},
        context,
    ) is None


def test_enabled_api_requires_key_but_worker_does_not_receive_it() -> None:
    base = {"database_url": "postgresql+asyncpg://unused/unused", "session_secret": "dev-secret"}
    with pytest.raises(ValidationError, match="SILICONFLOW_API_KEY"):
        Settings(**base, ai_enabled=True, ai_runtime_role="api", siliconflow_api_key="")
    with pytest.raises(ValidationError, match="DEEPSEEK_API_KEY"):
        Settings(
            **base,
            ai_enabled=True,
            ai_runtime_role="api",
            ai_provider="deepseek",
            deepseek_api_key="",
        )
    worker = Settings(**base, ai_enabled=True, ai_runtime_role="worker", siliconflow_api_key="")
    assert worker.ai_enabled is True
    assert worker.siliconflow_api_key.get_secret_value() == ""
    deepseek_worker = Settings(
        **base,
        ai_enabled=True,
        ai_runtime_role="worker",
        ai_provider="deepseek",
        deepseek_api_key="",
    )
    assert deepseek_worker.ai_provider == "deepseek"
    assert deepseek_worker.deepseek_api_key.get_secret_value() == ""


def test_siliconflow_key_is_redacted_from_settings_and_validation_errors() -> None:
    sentinel = "sf-sentinel-must-never-appear"
    base = {
        "database_url": "postgresql+asyncpg://unused/unused",
        "session_secret": "dev-secret",
        "ai_enabled": True,
        "ai_runtime_role": "api",
        "siliconflow_api_key": sentinel,
    }

    settings = Settings(**base)
    assert settings.siliconflow_api_key.get_secret_value() == sentinel
    assert sentinel not in repr(settings)
    assert sentinel not in repr(settings.model_dump())

    with pytest.raises(ValidationError) as raised:
        Settings(**base, app_env="production", session_cookie_secure=False)
    assert sentinel not in str(raised.value)
    assert sentinel not in repr(raised.value)


def test_suggestion_accepts_orbit_program_formula_patch() -> None:
    """A custom formula (orbitProgram formula/dsl) patch is accepted and returned."""
    from app.ai.models import validate_studio_suggestion

    result = validate_studio_suggestion({
        "patch": {
            "orbitProgram": {
                "type": "formula",
                "formula": {"type": "dsl", "source": "z*z*z+c"},
            },
        },
        "reason": "改用三次方公式增强结构",
    }, _context(member=True), )
    assert result is not None
    assert result["patch"]["orbitProgram"]["formula"]["source"] == "z*z*z+c"
    assert result["patch"]["orbitProgram"]["type"] == "formula"


def test_suggestion_rejects_invalid_orbit_program() -> None:
    from app.ai.models import validate_studio_suggestion

    # unknown program type -> rejected
    assert validate_studio_suggestion({
        "patch": {"orbitProgram": {"type": "sequence", "steps": []}},
        "reason": "非法轨道程序",
    }, _context(member=True)) is None
    # malformed dsl formula -> rejected
    assert validate_studio_suggestion({
        "patch": {"orbitProgram": {"type": "formula", "formula": {"type": "dsl"}}},
        "reason": "缺 source",
    }, _context(member=True)) is None
