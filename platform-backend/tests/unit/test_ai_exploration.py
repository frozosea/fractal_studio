"""Contracts for validated multi-candidate Studio exploration."""
from __future__ import annotations

from app.ai.exploration import validate_candidate_set


def _context(*, center: str = "-0.743643887037151000000000000000001") -> dict[str, object]:
    return {
        "member": False,
        "mode": "map",
        "output": {"width": 1600, "height": 1200},
        "spec": {
            "version": 1,
            "centerRe": float(center),
            "centerIm": 0.13182590420533,
            "centerReStr": center,
            "centerImStr": "0.131825904205330000000000000000002",
            "scale": 0.003,
            "iterations": 900,
            "variant": "mandelbrot",
            "colorMap": "inferno",
            "metric": "escape",
            "smooth": False,
            "colorMode": "eq_full",
            "cyclesPerOctave": 0.1,
            "rotationDeg": 0,
            "pairwiseCap": 64,
            "julia": False,
            "bailout": 2,
            "engine": "auto",
            "scalarType": "auto",
            "transitionMode": "off",
        },
        "capabilities": {
            "metrics": ["escape", "min_abs"],
            "engines": ["auto", "cuda"],
            "scalars": ["auto", "fp64"],
            "colorMaps": ["inferno", "viridis", "ember_blue", "twilight", "spectral1530"],
            "colorModes": ["direct", "eq_full", "eq_center"],
            "variants": ["mandelbrot"],
            "imageKinds": {
                "map": {
                    "enabled": True,
                    "metrics": ["escape", "min_abs"],
                    "engines": ["auto", "cuda"],
                    "scalars": ["auto", "fp64"],
                    "orbitProgram": True,
                },
                "transition": {
                    "enabled": False,
                    "metrics": [],
                    "engines": [],
                    "scalars": [],
                    "orbitProgram": False,
                },
            },
            "customGradient": {
                "enabled": True,
                "maxStops": 16,
                "kinds": ["map_image"],
            },
        },
    }


def test_location_candidates_preserve_exact_center_strings() -> None:
    result = validate_candidate_set({
        "axis": "position",
        "candidates": [
            {"label": "左侧枝干", "offsetX": -0.2, "offsetY": 0, "scaleFactor": 1, "reason": "查看左侧"},
            {"label": "上方分叉", "offsetX": 0, "offsetY": 0.2, "scaleFactor": 1, "reason": "查看上方"},
            {"label": "右下留白", "offsetX": 0.16, "offsetY": -0.12, "scaleFactor": 1, "reason": "平衡留白"},
        ],
    }, _context(), "location")

    assert result is not None
    assert result["kind"] == "candidate_set"
    assert result["mode"] == "location"
    assert len(result["baseSpecHash"]) == 64
    assert result["baseSpec"]["centerReStr"] == _context()["spec"]["centerReStr"]
    assert result["baseMode"] == "map"
    assert result["baseOutput"] == {"width": 1600, "height": 1200}
    first = result["candidates"][0]
    assert first["id"] == "A" and first["verification"] == "pending"
    assert first["patch"]["centerReStr"].startswith("-0.744443887037151")
    assert len(first["patch"]["centerReStr"]) > 20


def test_location_scale_axis_rejects_mixed_pan_without_hidden_iteration_change() -> None:
    mixed = {
        "axis": "scale",
        "candidates": [
            {"label": "a", "offsetX": 0.1, "offsetY": 0, "scaleFactor": 0.7, "reason": "a"},
            {"label": "b", "offsetX": 0, "offsetY": 0, "scaleFactor": 0.8, "reason": "b"},
            {"label": "c", "offsetX": 0, "offsetY": 0, "scaleFactor": 1.2, "reason": "c"},
        ],
    }
    assert validate_candidate_set(mixed, _context(), "location") is None
    mixed["candidates"][0]["offsetX"] = 0
    result = validate_candidate_set(mixed, _context(), "location")
    assert result is not None
    assert result["candidates"][0]["patch"] == {"scale": 0.0021}


def test_composition_candidates_apply_aspect_rotation_and_are_unique() -> None:
    context = _context()
    context["spec"]["rotationDeg"] = 30
    result = validate_candidate_set({
        "candidates": [
            {"label": "对角线", "offsetX": 0.1, "offsetY": 0.1, "scaleFactor": 1, "rotationDelta": 0, "reason": "沿对角线"},
            {"label": "收紧", "offsetX": 0, "offsetY": 0, "scaleFactor": 0.8, "rotationDelta": 0, "reason": "突出主体"},
            {"label": "旋转", "offsetX": 0, "offsetY": 0, "scaleFactor": 1, "rotationDelta": 12, "reason": "强化动势"},
        ],
    }, context, "composition")

    assert result is not None
    assert result["candidates"][0]["patch"]["centerReStr"] != context["spec"]["centerReStr"]
    assert result["candidates"][2]["patch"]["rotationDeg"] == 42


def test_color_candidates_support_builtins_and_bounded_custom_gradients() -> None:
    result = validate_candidate_set({
        "candidates": [
            {"label": "深海", "patch": {"colorMap": "ember_blue"}, "reason": "冷暖层次"},
            {"label": "暮光", "patch": {"colorMap": "twilight"}, "reason": "循环柔和"},
            {"label": "翠色", "patch": {"colorMap": "viridis"}, "reason": "暗部清晰"},
            {"label": "定制", "patch": {"colorProgram": {
                "wrap": "repeat", "cycles": 2, "phase": 0.1,
                "interiorColor": "#050505", "invalidColor": "#ff00ff",
                "stops": [
                    {"at": 0, "color": "#071426"},
                    {"at": 0.5, "color": "#37b4c3"},
                    {"at": 1, "color": "#fff6d2"},
                ],
            }}, "reason": "自定义深海高光"},
        ],
    }, _context(), "color")

    assert result is not None
    first = result["candidates"][0]["patch"]
    assert first == {"colorMap": "ember_blue"}
    custom = result["candidates"][3]["patch"]
    assert custom["colorMap"] is None
    assert custom["colorMode"] == "direct"
    assert custom["colorProgram"]["stops"][1]["color"] == "#37b4c3"


def test_color_candidates_fail_closed_on_structure_change_or_unsupported_palette() -> None:
    raw = {
        "candidates": [
            {"label": str(index), "patch": {"colorMap": palette}, "reason": "test"}
            for index, palette in enumerate(["ember_blue", "twilight", "viridis", "not-real"])
        ]
    }
    assert validate_candidate_set(raw, _context(), "color") is None
    raw["candidates"][3]["patch"] = {"centerRe": 0}
    assert validate_candidate_set(raw, _context(), "color") is None


def test_candidate_set_rejects_duplicates_and_untrusted_capabilities() -> None:
    raw = {
        "axis": "position",
        "candidates": [
            {"label": str(index), "offsetX": 0.1, "offsetY": 0, "scaleFactor": 1, "reason": "same"}
            for index in range(3)
        ],
    }
    assert validate_candidate_set(raw, _context(), "location") is None
    context = _context()
    context["capabilities"] = {}
    assert validate_candidate_set(raw, context, "location") is None


def test_candidate_set_rejects_untrusted_mode_or_output_identity() -> None:
    raw = {
        "axis": "position",
        "candidates": [
            {"label": "a", "offsetX": -0.2, "offsetY": 0, "scaleFactor": 1, "reason": "a"},
            {"label": "b", "offsetX": 0, "offsetY": 0.2, "scaleFactor": 1, "reason": "b"},
            {"label": "c", "offsetX": 0.2, "offsetY": 0, "scaleFactor": 1, "reason": "c"},
        ],
    }
    context = _context()
    context["mode"] = "julia"
    assert validate_candidate_set(raw, context, "location") is None
    context = _context()
    context["output"] = {"width": 1600.5, "height": 1200}
    assert validate_candidate_set(raw, context, "location") is None


def test_candidate_set_rejects_semantically_tiny_or_wrong_typed_changes() -> None:
    tiny = {
        "axis": "position",
        "candidates": [
            {"label": "a", "offsetX": 0.001, "offsetY": 0, "scaleFactor": 1, "reason": "a"},
            {"label": "b", "offsetX": 0.1, "offsetY": 0, "scaleFactor": 1, "reason": "b"},
            {"label": "c", "offsetX": -0.1, "offsetY": 0, "scaleFactor": 1, "reason": "c"},
        ],
    }
    assert validate_candidate_set(tiny, _context(), "location") is None
    tiny["candidates"][0] = {
        "label": {"not": "text"},
        "offsetX": 0.2,
        "offsetY": 0,
        "scaleFactor": 1,
        "reason": ["not", "text"],
    }
    assert validate_candidate_set(tiny, _context(), "location") is None
    tiny["candidates"][0] = {
        "label": "huge",
        "offsetX": 10**1000,
        "offsetY": 0,
        "scaleFactor": 1,
        "reason": "must fail closed",
    }
    assert validate_candidate_set(tiny, _context(), "location") is None


def test_candidates_enforce_cross_field_membership_and_compute_contracts() -> None:
    context = _context()
    context["spec"]["orbitProgram"] = {
        "type": "formula",
        "formula": {"type": "builtin", "id": "mandelbrot"},
    }
    context["mode"] = "formula"
    context["capabilities"]["orbitPrograms"] = {"formula": True, "sequence": True}
    raw = {
        "candidates": [
            {"label": "a", "patch": {"colorMap": "ember_blue"}, "reason": "a"},
            {"label": "b", "patch": {"colorMap": "twilight"}, "reason": "b"},
            {"label": "c", "patch": {"colorMap": "viridis"}, "reason": "c"},
            {
                "label": "bad",
                "patch": {"metric": "min_abs", "colorMode": "direct"},
                "reason": "orbit programs require escape",
            },
        ],
    }
    assert validate_candidate_set(raw, context, "color") is None
    # Formula/sequence modes are member-only even when Compute advertises them.
    raw["candidates"][3] = {
        "label": "d",
        "patch": {"colorMap": "spectral1530"},
        "reason": "d",
    }
    assert validate_candidate_set(raw, context, "color") is None
    context["member"] = True
    assert validate_candidate_set(raw, context, "color") is not None


def test_color_candidates_reject_null_palette_and_normalize_equivalent_phase() -> None:
    raw = {
        "candidates": [
            {"label": "empty", "patch": {"colorMap": None}, "reason": "no fallback"},
            {"label": "b", "patch": {"colorMap": "twilight"}, "reason": "b"},
            {"label": "c", "patch": {"colorMap": "viridis"}, "reason": "c"},
            {"label": "d", "patch": {"colorMap": "spectral1530"}, "reason": "d"},
        ],
    }
    assert validate_candidate_set(raw, _context(), "color") is None

    program = {
        "wrap": "repeat",
        "cycles": 2,
        "phase": 0.1,
        "interiorColor": "#AABBCC",
        "invalidColor": "#FF00FF",
        "stops": [{"at": 0, "color": "#000000"}, {"at": 1, "color": "#FFFFFF"}],
    }
    raw["candidates"][0] = {"label": "a", "patch": {"colorProgram": program}, "reason": "a"}
    raw["candidates"][1] = {
        "label": "same",
        "patch": {"colorProgram": {**program, "phase": 1.1}},
        "reason": "same rendered phase",
    }
    assert validate_candidate_set(raw, _context(), "color") is None
