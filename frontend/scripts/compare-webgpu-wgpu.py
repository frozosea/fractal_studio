#!/usr/bin/env python3
"""WebGPU WGSL parity runner (Python wgpu).

Extracts the exact FRACTAL_SHADER WGSL source from
frontend/src/lib/fractal/webgpu-renderer.ts, runs it through wgpu-native
(Vulkan, typically the NVIDIA adapter) for the same 7 cases as
compare-webgpu-render.mjs, and writes the raw RGBA output to
/tmp/wgpu-parity/<case>.rgba for the Node comparator to diff against the
C++-verified CPU fp64 core.

Usage:
  /tmp/wgpu-venv/bin/python scripts/compare-webgpu-wgpu.py
"""

import json
import math
import os
import re
import struct
import sys
from pathlib import Path

import wgpu

ROOT = Path(__file__).resolve().parent.parent
RENDERER = ROOT / "src" / "lib" / "fractal" / "webgpu-renderer.ts"
OUT_DIR = Path("/tmp/wgpu-parity")

# ---------------------------------------------------------------------------
# Extract the WGSL shader source verbatim from the TypeScript module.
# ---------------------------------------------------------------------------

source = RENDERER.read_text(encoding="utf-8")
match = re.search(r"FRACTAL_SHADER\s*=\s*/\*\s*wgsl\s*\*/\s*`(.*?)`", source, re.S)
if not match:
    sys.exit("FRACTAL_SHADER not found in " + str(RENDERER))
WGSL = match.group(1)

# ---------------------------------------------------------------------------
# Index tables mirroring webgpu-renderer.ts.
# ---------------------------------------------------------------------------

VARIANT_INDEX = {
    "mandelbrot": 0, "tricorn": 1, "burning_ship": 2, "celtic": 3, "heart": 4,
    "buffalo": 5, "perp_buffalo": 6, "celtic_ship": 7, "mandelceltic": 8, "perp_ship": 9,
}
METRIC_INDEX = {"escape": 0, "min_abs": 1, "max_abs": 2, "envelope": 3}
PALETTE_INDEX = {
    "classic_cos": 0, "mod17": 1, "hsv_wheel": 2, "tri765": 3, "grayscale": 4,
    "hs_rainbow": 5, "inferno": 6, "viridis": 7, "twilight": 8, "ember_blue": 9,
    "spectral1530": 10,
}

CASES = [
    ("mandel_escape", 128, 96, {"centerRe": -0.75, "centerIm": 0, "scale": 3, "iterations": 180, "variant": "mandelbrot", "metric": "escape", "colorMap": "classic_cos", "smooth": False, "rotationDeg": 0, "julia": False, "juliaRe": 0, "juliaIm": 0, "bailout": 2}),
    ("ship_julia_smooth", 128, 96, {"centerRe": -0.64, "centerIm": 0.03, "scale": 2.4, "iterations": 180, "variant": "burning_ship", "metric": "escape", "colorMap": "viridis", "smooth": True, "rotationDeg": 19, "julia": True, "juliaRe": -0.8, "juliaIm": 0.156, "bailout": 2}),
    ("min_abs_hs_rainbow", 128, 96, {"centerRe": -0.75, "centerIm": 0, "scale": 3, "iterations": 200, "variant": "mandelbrot", "metric": "min_abs", "colorMap": "hs_rainbow", "smooth": False, "rotationDeg": 0, "julia": False, "juliaRe": 0, "juliaIm": 0, "bailout": 2}),
    ("max_abs_twilight_smooth", 128, 96, {"centerRe": -0.75, "centerIm": 0, "scale": 3, "iterations": 200, "variant": "tricorn", "metric": "max_abs", "colorMap": "twilight", "smooth": True, "rotationDeg": 0, "julia": False, "juliaRe": 0, "juliaIm": 0, "bailout": 2}),
    ("envelope_inferno_rotated", 128, 96, {"centerRe": -0.5, "centerIm": 0, "scale": 2.2, "iterations": 220, "variant": "heart", "metric": "envelope", "colorMap": "inferno", "smooth": False, "rotationDeg": 37, "julia": False, "juliaRe": 0, "juliaIm": 0, "bailout": 2}),
    ("perp_ship_spectral_julia", 128, 96, {"centerRe": -0.6, "centerIm": 0.1, "scale": 2, "iterations": 220, "variant": "perp_ship", "metric": "escape", "colorMap": "spectral1530", "smooth": True, "rotationDeg": -90, "julia": True, "juliaRe": -0.8, "juliaIm": 0.156, "bailout": 2}),
    ("deep_zoom_fp32_stress", 128, 96, {"centerRe": -0.7435, "centerIm": 0.1314, "scale": 0.0002, "iterations": 600, "variant": "mandelbrot", "metric": "escape", "colorMap": "classic_cos", "smooth": False, "rotationDeg": 0, "julia": False, "juliaRe": 0, "juliaIm": 0, "bailout": 2}),
]


def uniform_bytes(spec: dict, width: int, height: int) -> bytes:
    angle = spec["rotationDeg"] * math.pi / 180
    floats = [
        spec["centerRe"], spec["centerIm"], spec["scale"], width / height,
        math.cos(angle), math.sin(angle), spec["bailout"], spec["bailout"] ** 2,
        spec["juliaRe"], spec["juliaIm"], 0.0, 0.0,
    ]
    u32s = [
        width, height, spec["iterations"],
        VARIANT_INDEX[spec["variant"]], METRIC_INDEX[spec["metric"]],
        PALETTE_INDEX[spec["colorMap"]], 1 if spec["smooth"] else 0,
        1 if spec["julia"] else 0,
    ]
    return struct.pack("<12f", *floats) + struct.pack("<8I", *u32s)


def main() -> int:
    adapter = wgpu.gpu.request_adapter_sync(power_preference="high-performance")
    if adapter is None:
        print("no wgpu adapter (Vulkan unavailable?)")
        return 1
    info = dict(adapter.info)
    print(f"adapter: {info.get('device', 'unknown')} ({info.get('adapter_type', '?')} / {info.get('backend_type', '?')})")

    device = adapter.request_device_sync()

    shader = device.create_shader_module(code=WGSL)
    pipeline = device.create_compute_pipeline(
        layout="auto",
        compute={"module": shader, "entry_point": "main"},
    )

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for name, width, height, spec in CASES:
        params = device.create_buffer(
            size=80,
            usage=wgpu.BufferUsage.UNIFORM | wgpu.BufferUsage.COPY_DST,
        )
        pixels = device.create_buffer(
            size=width * height * 4,
            usage=wgpu.BufferUsage.STORAGE | wgpu.BufferUsage.COPY_SRC,
        )
        readback = device.create_buffer(
            size=width * height * 4,
            usage=wgpu.BufferUsage.COPY_DST | wgpu.BufferUsage.MAP_READ,
        )
        device.queue.write_buffer(params, 0, uniform_bytes(spec, width, height))

        bind_group = device.create_bind_group(
            layout=pipeline.get_bind_group_layout(0),
            entries=[
                {"binding": 0, "resource": params},
                {"binding": 1, "resource": pixels},
            ],
        )
        encoder = device.create_command_encoder()
        pass_ = encoder.begin_compute_pass()
        pass_.set_pipeline(pipeline)
        pass_.set_bind_group(0, bind_group)
        pass_.dispatch_workgroups(math.ceil(width / 8), math.ceil(height / 8))
        pass_.end()
        encoder.copy_buffer_to_buffer(pixels, 0, readback, 0, width * height * 4)
        device.queue.submit([encoder.finish()])

        readback.map_sync(wgpu.MapMode.READ)
        data = bytes(readback.read_mapped())
        readback.unmap()

        # u32 packed (r | g<<8 | b<<16) -> RGBA bytes, alpha 255, like the TS renderer.
        packed = struct.unpack(f"<{width * height}I", data)
        rgba = bytearray(width * height * 4)
        for index, color in enumerate(packed):
            offset = index * 4
            rgba[offset] = color & 0xFF
            rgba[offset + 1] = (color >> 8) & 0xFF
            rgba[offset + 2] = (color >> 16) & 0xFF
            rgba[offset + 3] = 255
        target = OUT_DIR / f"{name}.rgba"
        target.write_bytes(bytes(rgba))
        print(f"wrote {target} ({width * height} px)")

    return 0


if __name__ == "__main__":
    sys.exit(main())
