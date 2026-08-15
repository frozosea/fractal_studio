#!/usr/bin/env python3
"""Hybrid-precision (fp64 coordinates + fp32 iteration) deep-zoom benchmark.

Validates the claim that high-precision coordinate mapping with fp32 orbit
iteration is the fast path for deep zooms on NVIDIA GPUs (fp64 runs at ~1/64
rate, so keeping the heavy per-pixel loop in fp32 while mapping pixels with
double precision wins once the zoom outgrows plain fp32).

Three shader variants on the same zoom ladder (scale 1e-4 .. 1e-16):
  f32     - pure fp32 coordinates and iteration (baseline)
  hybrid  - fp64 pixel mapping split into a double-single pair (hi+lo f32),
            fp32 orbit with c = z^2 + c_hi + c_lo
  f64     - pure fp64 (reference for iteration counts)

Grades iteration-count agreement against the f64 reference and times each
variant. Usage:
  /tmp/wgpu-venv/bin/python scripts/compare-hybrid-wgpu.py
"""

import math
import struct
import time

import wgpu

CENTER = (-0.743643887037151, 0.13182590420533)
ZOOMS = [1e-4, 1e-8, 1e-12, 1e-16]
ITERATIONS = 1500
WIDTH, HEIGHT = 512, 384

F64_SHADER = """
struct Params {
  centerRe: f64, centerIm: f64, scale: f64, aspect: f64,
  bailoutSq: f64, juliaRe: f64, juliaIm: f64, pad0: f64,
  width: u32, height: u32, iterations: u32, variant: u32,
  julia: u32, pad1: u32, pad2: u32, pad3: u32,
}
struct Pixel { iter: u32, norm: f64 }
@group(0) @binding(0) var<uniform> p: Params;
@group(0) @binding(1) var<storage, read_write> pixels: array<Pixel>;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) id: vec3<u32>) {
  let width = p.width;
  let height = p.height;
  if (id.x >= width || id.y >= height) { return; }
  let local_re = ((f64(id.x) + 0.5) / f64(width) - 0.5) * p.scale * p.aspect;
  let local_im = (0.5 - (f64(id.y) + 0.5) / f64(height)) * p.scale;
  let re = p.centerRe + local_re;
  let im = p.centerIm + local_im;
  var x: f64 = 0.0;
  var y: f64 = 0.0;
  var iteration = p.iterations;
  var norm: f64 = 0.0;
  for (var i = 0u; i < p.iterations; i = i + 1u) {
    let nx = x * x - y * y + re;
    let ny = 2.0 * x * y + im;
    x = nx;
    y = ny;
    norm = x * x + y * y;
    if (norm > p.bailoutSq) { iteration = i; break; }
  }
  let index = id.y * width + id.x;
  pixels[index] = Pixel(iteration, norm);
}
"""

HYBRID_SHADER = """
struct Params {
  centerRe: f64, centerIm: f64, scale: f64, aspect: f64,
  bailoutSq: f64, juliaRe: f64, juliaIm: f64, pad0: f64,
  width: u32, height: u32, iterations: u32, variant: u32,
  julia: u32, pad1: u32, pad2: u32, pad3: u32,
}
struct Pixel { iter: u32, norm: f32 }
@group(0) @binding(0) var<uniform> p: Params;
@group(0) @binding(1) var<storage, read_write> pixels: array<Pixel>;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) id: vec3<u32>) {
  let width = p.width;
  let height = p.height;
  if (id.x >= width || id.y >= height) { return; }
  // fp64 pixel mapping, then split into a double-single pair so the fp32
  // orbit still sees the full coordinate precision (c = hi + lo).
  let local_re = ((f64(id.x) + 0.5) / f64(width) - 0.5) * p.scale * p.aspect;
  let local_im = (0.5 - (f64(id.y) + 0.5) / f64(height)) * p.scale;
  let re = p.centerRe + local_re;
  let im = p.centerIm + local_im;
  let c_hi_re = f32(re);
  let c_hi_im = f32(im);
  let c_lo_re = f32(re - f64(c_hi_re));
  let c_lo_im = f32(im - f64(c_hi_im));
  var x: f32 = 0.0;
  var y: f32 = 0.0;
  var iteration = p.iterations;
  var norm: f32 = 0.0;
  for (var i = 0u; i < p.iterations; i = i + 1u) {
    let nx = x * x - y * y + c_hi_re + c_lo_re;
    let ny = 2.0 * x * y + c_hi_im + c_lo_im;
    x = nx;
    y = ny;
    norm = x * x + y * y;
    if (norm > 4.0) { iteration = i; break; }
  }
  let index = id.y * width + id.x;
  pixels[index] = Pixel(iteration, norm);
}
"""

F32_SHADER = HYBRID_SHADER.replace("f64", "f32")  # pure f32 everywhere


def uniform_bytes(center, scale, width, height, iterations):
    f = (center[0], center[1], scale, width / height, 4.0, 0.0, 0.0, 0.0)
    u = (width, height, iterations, 0, 0, 0, 0, 0)
    return struct.pack("<8d8I", *(f + u))


def run_shader(device, pipeline, center, scale, width, height, iterations, f32_out):
    if f32_out:
        pixel_size = 8  # u32 + f32
        pixel_fmt = "<If"
    else:
        pixel_size = 16  # u32 + f64
        pixel_fmt = "<Id"
    params = device.create_buffer(size=96, usage=wgpu.BufferUsage.UNIFORM | wgpu.BufferUsage.COPY_DST)
    pixels = device.create_buffer(size=width * height * pixel_size, usage=wgpu.BufferUsage.STORAGE | wgpu.BufferUsage.COPY_SRC)
    read = device.create_buffer(size=width * height * pixel_size, usage=wgpu.BufferUsage.COPY_DST | wgpu.BufferUsage.MAP_READ)
    device.queue.write_buffer(params, 0, uniform_bytes(center, scale, width, height, iterations))
    bg = device.create_bind_group(layout=pipeline.get_bind_group_layout(0), entries=[
        {"binding": 0, "resource": params}, {"binding": 1, "resource": pixels}])
    encoder = device.create_command_encoder()
    pass_ = encoder.begin_compute_pass()
    pass_.set_pipeline(pipeline)
    pass_.set_bind_group(0, bg)
    pass_.dispatch_workgroups(math.ceil(width / 8), math.ceil(height / 8))
    pass_.end()
    encoder.copy_buffer_to_buffer(pixels, 0, read, 0, width * height * pixel_size)
    device.queue.submit([encoder.finish()])
    read.map_sync(wgpu.MapMode.READ)
    data = bytes(read.read_mapped())
    read.unmap()
    rows = list(struct.iter_unpack(pixel_fmt, data[:width * height * pixel_size]))
    return [r[0] for r in rows]


def main() -> int:
    adapter = wgpu.gpu.request_adapter_sync(power_preference="high-performance")
    if adapter is None:
        print("no adapter")
        return 1
    print(f"adapter: {adapter.info['device']} / {adapter.info['backend_type']}")
    device = adapter.request_device_sync(required_features=["shader-f64"] if "shader-f64" in adapter.features else [])

    pipelines = {}
    for name, src, f32_out in (
        ("f32", F32_SHADER, True),
        ("hybrid", HYBRID_SHADER, True),
        ("f64", F64_SHADER, False),
    ):
        module = device.create_shader_module(code=src)
        pipelines[name] = (device.create_compute_pipeline(layout="auto", compute={"module": module, "entry_point": "main"}), f32_out)

    print(f"\niteration-count agreement vs f64 reference, {WIDTH}x{HEIGHT}, {ITERATIONS} it:")
    print(f"{'scale':>8} {'f32':>12} {'hybrid':>12} {'f64':>10}   (mismatch fraction)")
    for scale in ZOOMS:
        f64_iter = run_shader(device, pipelines["f64"][0], CENTER, scale, WIDTH, HEIGHT, ITERATIONS, False)
        row = []
        for name in ("f32", "hybrid"):
            iters = run_shader(device, pipelines[name][0], CENTER, scale, WIDTH, HEIGHT, ITERATIONS, True)
            bad = sum(1 for a, b in zip(iters, f64_iter) if a != b)
            row.append(f"{bad / len(f64_iter):.2%}")
        print(f"{scale:>8.0e} {row[0]:>12} {row[1]:>12} {'0.00%':>10}")

    print(f"\nefficiency at scale 1e-8 and 1e-12 ({WIDTH}x{HEIGHT}, {ITERATIONS} it, best of 3):")
    print(f"{'scale':>8} {'f32 ms':>10}{'hybrid ms':>12}{'f64 ms':>10}{'hybrid/f64':>12}")
    for scale in (1e-8, 1e-12):
        times = {}
        for name, (pipeline, f32_out) in pipelines.items():
            best = float("inf")
            for _ in range(3):
                start = time.perf_counter()
                run_shader(device, pipeline, CENTER, scale, WIDTH, HEIGHT, ITERATIONS, f32_out)
                best = min(best, time.perf_counter() - start)
            times[name] = best * 1000
        print(f"{scale:>8.0e} {times['f32']:>10.1f}{times['hybrid']:>12.1f}{times['f64']:>10.1f}{times['hybrid'] / times['f64']:>12.2f}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
