from __future__ import annotations

from typing import Any


def builtin_formula(identifier: str = "mandelbrot") -> dict[str, Any]:
    return {"type": "formula", "formula": {"type": "builtin", "id": identifier}}


def sequence_program() -> dict[str, Any]:
    return {
        "type": "sequence",
        "repeat": True,
        "steps": [
            {"span": 1, "program": builtin_formula("mandelbrot")},
            {"span": 1, "program": builtin_formula("burning_ship")},
        ],
    }


def map_payload(
    *, orbit: bool = False, size: int = 64, iterations: int = 32,
    engine: str = "openmp", scalar: str = "fp64",
) -> dict[str, Any]:
    payload: dict[str, Any] = {
        "centerRe": -0.75, "centerIm": 0.0, "scale": 3.0,
        "width": size, "height": size, "iterations": iterations,
        "variant": "mandelbrot", "engine": engine, "scalarType": scalar,
    }
    if orbit:
        payload["orbitProgram"] = sequence_program()
    return payload


def strict_dsl_payload() -> dict[str, Any]:
    return {
        "centerRe": 100.0, "centerIm": 0.0, "scale": 1e-6,
        "viewportAspect": 1.0, "width": 1, "height": 1, "iterations": 5,
        "julia": True, "juliaRe": 0.0, "juliaIm": 0.0, "metric": "escape",
        "orbitProgram": {
            "type": "formula", "formula": {"type": "dsl", "source": "z"},
        },
    }


def hs_mesh_payload() -> dict[str, Any]:
    return {
        "centerRe": -0.75, "centerIm": 0.0, "scale": 3.0,
        "resolution": 8, "iterations": 16, "metric": "min_abs",
        "heightScale": 0.5, "heightClamp": 2.0,
        "orbitProgram": sequence_program(),
    }


def hs_field_payload() -> dict[str, Any]:
    payload = hs_mesh_payload()
    payload.pop("heightScale")
    return payload


def ln_map_payload(*, color_mode: str = "escape", orbit: Any = None) -> dict[str, Any]:
    payload: dict[str, Any] = {
        "centerRe": -0.75, "centerIm": 0.0, "variant": "mandelbrot",
        "widthS": 128, "depthOctaves": 1.0, "lnMapExtraOctaves": 2.0,
        "iterations": 32, "lnMapColorMode": color_mode, "colorMap": "classic_cos",
    }
    if orbit is not None:
        payload["orbitProgram"] = orbit
    return payload


def zoom_payload() -> dict[str, Any]:
    return {
        "centerRe": -0.75, "centerIm": 0.0,
        "width": 128, "height": 128, "previewWidth": 64, "previewHeight": 64,
        "widthS": 128, "previewLnMapWidthS": 128,
        "depthOctaves": 0.05, "secondsPerOctave": 1.0,
        "fps": 1, "iterations": 16, "lnMapColorMode": "escape",
        "colorMap": "classic_cos", "cudaWarp": False,
        "orbitProgram": sequence_program(),
    }


def reusable_zoom_payload() -> dict[str, Any]:
    return {**zoom_payload(), "depthOctaves": 1.0}


def legacy_zoom_payload(source_id: str) -> dict[str, Any]:
    return {
        "lnMapArtifactId": f"{source_id}:ln_map.png",
        "width": 128, "height": 128, "fps": 1,
        "depthOctaves": 1.0, "secondsPerOctave": 1.0,
        "cudaWarp": False,
    }


def transition_mesh_payload() -> dict[str, Any]:
    return {
        "centerX": 0.0, "centerY": 0.0, "centerZ": 0.0,
        "extent": 2.0, "resolution": 16, "iterations": 32,
        "bailout": 2.0, "transitionFrom": "mandelbrot",
        "transitionTo": "burning_ship", "engine": "openmp",
        "scalarType": "fp32", "iso": 0.5,
    }


def transition_voxels_payload() -> dict[str, Any]:
    payload = transition_mesh_payload()
    payload["iso"] = 0.48
    return payload


def transition_video_payload() -> dict[str, Any]:
    return {
        "centerRe": -0.75, "centerIm": 0.0, "scale": 3.0,
        "width": 128, "height": 128, "iterations": 16,
        "fps": 1, "durationSec": 2.0,
        "thetaStartDeg": 0.0, "thetaEndDeg": 90.0,
        "transitionFrom": "mandelbrot", "transitionTo": "burning_ship",
        "engine": "openmp", "scalarType": "fp64",
    }


def special_points_enumerate_payload() -> dict[str, Any]:
    return {
        "kind": "center", "periodMin": 1, "periodMax": 1,
        "maxNewtonIter": 20, "maxSeedBatches": 1, "seedsPerBatch": 32,
        "includeVariantExistence": False, "includeRejectedDebug": False,
    }


def special_points_search_payload() -> dict[str, Any]:
    return {
        "kind": "center", "periodMin": 1, "periodMax": 1,
        "seedBudget": 32, "maxNewtonIter": 20,
        "visibleOnly": True, "includeVariantCompatibility": False,
        "viewport": {
            "centerRe": -0.75, "centerIm": 0.0, "scale": 3.0,
            "rotationDeg": 0.0, "width": 64, "height": 64,
        },
    }


def benchmark_payload() -> dict[str, Any]:
    return {
        "centerRe": -0.75, "centerIm": 0.0, "scale": 1.5,
        "width": 64, "height": 64, "iterations": 8,
        "samples": 1, "warmup": 0, "replaceCache": False,
        "workload": "pytest",
    }
