#!/usr/bin/env python3
"""Perturbation deep-zoom benchmark: high-precision reference + fp32 delta orbit.

The classic deep-zoom trick (Kalles Fraktaler style): compute the reference
orbit Z_ref at the view center in full precision (Python f64 here; MPFR in
the backend), then iterate the *delta* dz = z - Z_ref in fp32 on the GPU:

  dz' = 2*Z_ref[i]*dz + dz^2 + dc        (dc = c - c_ref is small)

Because dz and dc are small, fp32 keeps full relative precision, so iteration
counts track the f64 reference even at extreme zoom, while the per-pixel loop
runs at fp32 speed. This validates the "high-precision coords + fp32 iteration
is the fast deep-zoom path on NVIDIA GPUs" claim (fp64 is 1/64 rate).

Usage:
  /tmp/wgpu-venv/bin/python scripts/compare-perturb-wgpu.py
"""

import math
import struct
import time

import wgpu

CENTER = (-0.743643887037151, 0.13182590420533)
ZOOMS = [1e-4, 1e-8, 1e-12]
ITERATIONS = 1500
WIDTH, HEIGHT = 512, 384


def reference_orbit(center, iterations):
    x = y = 0.0
    cx, cy = center
    xs, ys = [], []
    for _ in range(iterations):
        nx = x * x - y * y + cx
        ny = 2.0 * x * y + cy
        x, y = nx, ny
        xs.append(x)
        ys.append(y)
        if x * x + y * y > 4.0:
            break
    return xs, ys


PERTURB_SHADER = """
struct Params {
  centerRe: f64, centerIm: f64, scale: f64, aspect: f64,
  bailoutSq: f64, pad1: f64, pad2: f64, pad3: f64,
  width: u32, height: u32, iterations: u32, refLen: u32,
  julia: u32, pad5: u32, pad6: u32, pad7: u32,
}
struct Pixel { iter: u32, norm: f32 }
@group(0) @binding(0) var<uniform> p: Params;
@group(0) @binding(1) var<storage, read_write> refs: array<vec2<f64>>;
@group(0) @binding(2) var<storage, read_write> pixels: array<Pixel>;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) id: vec3<u32>) {
  let width = p.width;
  let height = p.height;
  if (id.x >= width || id.y >= height) { return; }
  // dc = c - c_ref is just the pixel offset (f64), then f32: small value,
  // full fp32 relative precision.
  let local_re = ((f64(id.x) + 0.5) / f64(width) - 0.5) * p.scale * p.aspect;
  let local_im = (0.5 - (f64(id.y) + 0.5) / f64(height)) * p.scale;
  let dc_re = f32(local_re);
  let dc_im = f32(local_im);
  var dx: f32 = 0.0;
  var dy: f32 = 0.0;
  var iteration = p.iterations;
  var norm: f32 = 0.0;
  let n = min(p.iterations, p.refLen);
  for (var i = 0u; i < n; i = i + 1u) {
    let zx = refs[i].x;
    let zy = refs[i].y;
    // dz' = 2*Z*dz + dz^2 + dc
    let t_re = dx * dx - dy * dy + dc_re;
    let t_im = 2.0 * dx * dy + dc_im;
    dx = 2.0 * f32(zx) * dx - 2.0 * f32(zy) * dy + t_re;
    dy = 2.0 * f32(zx) * dy + 2.0 * f32(zy) * dx + t_im;
    // escape when |Z + dz|^2 > 4, Z kept in f64
    let zre = zx + f64(dx);
    let zim = zy + f64(dy);
    norm = f32(zre * zre + zim * zim);
    if (norm > 4.0) { iteration = i; break; }
  }
  let index = id.y * width + id.x;
  pixels[index] = Pixel(iteration, norm);
}
"""

F64_SHADER = """
struct Params {
  centerRe: f64, centerIm: f64, scale: f64, aspect: f64,
  bailoutSq: f64, pad1: f64, pad2: f64, pad3: f64,
  width: u32, height: u32, iterations: u32, refLen: u32,
  julia: u32, pad5: u32, pad6: u32, pad7: u32,
}
struct Pixel { iter: u32, norm: f64 }
@group(0) @binding(0) var<uniform> p: Params;
@group(0) @binding(2) var<storage, read_write> pixels: array<Pixel>;

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
    if (norm > 4.0) { iteration = i; break; }
  }
  let index = id.y * width + id.x;
  pixels[index] = Pixel(iteration, norm);
}
"""


def uniform_bytes(center, scale, width, height, iterations, ref_len):
    f = (center[0], center[1], scale, width / height, 4.0, 0.0, 0.0, 0.0)
    u = (width, height, iterations, ref_len, 0, 0, 0, 0)
    return struct.pack("<8d8I", *(f + u))


def run_shader(device, pipeline, refs_bytes, center, scale, width, height,
               iterations, ref_len, f32_out, uses_refs):
    if f32_out:
        pixel_size = 8
        pixel_fmt = "<If"
    else:
        pixel_size = 16
        pixel_fmt = "<I4xd"  # u32 + 4 pad + f64 = 16
    params = device.create_buffer(size=96, usage=wgpu.BufferUsage.UNIFORM | wgpu.BufferUsage.COPY_DST)
    pixels = device.create_buffer(size=width * height * pixel_size, usage=wgpu.BufferUsage.STORAGE | wgpu.BufferUsage.COPY_SRC)
    read = device.create_buffer(size=width * height * pixel_size, usage=wgpu.BufferUsage.COPY_DST | wgpu.BufferUsage.MAP_READ)
    device.queue.write_buffer(params, 0, uniform_bytes(center, scale, width, height, iterations, ref_len))
    entries = [{"binding": 0, "resource": params}]
    if uses_refs:
        refs = device.create_buffer(size=ref_len * 16, usage=wgpu.BufferUsage.STORAGE | wgpu.BufferUsage.COPY_DST)
        device.queue.write_buffer(refs, 0, refs_bytes)
        entries.append({"binding": 1, "resource": refs})
    entries.append({"binding": 2, "resource": pixels})
    bg = device.create_bind_group(layout=pipeline.get_bind_group_layout(0), entries=entries)
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

    xs, ys = reference_orbit(CENTER, ITERATIONS)
    print(f"reference orbit length: {len(xs)}/{ITERATIONS} (escaped={len(xs) < ITERATIONS})")
    refs_bytes = struct.pack(f"<{len(xs)}d{len(xs)}d", *xs, *ys)
    ref_len = len(xs)

    pipelines = {}
    for name, src, f32_out, uses_refs in (
        ("perturb", PERTURB_SHADER, True, True),
        ("f64", F64_SHADER, False, False),
    ):
        module = device.create_shader_module(code=src)
        pipelines[name] = (
            device.create_compute_pipeline(layout="auto", compute={"module": module, "entry_point": "main"}),
            f32_out, uses_refs,
        )

    print(f"\niteration-count agreement vs f64 reference, {WIDTH}x{HEIGHT}, {ITERATIONS} it:")
    print(f"{'scale':>8} {'perturb':>12} {'f64':>10}   (mismatch fraction)")
    for scale in ZOOMS:
        f64_iter = run_shader(device, pipelines["f64"][0], refs_bytes, CENTER, scale, WIDTH, HEIGHT, ITERATIONS, ref_len, *pipelines["f64"][1:])
        p_iter = run_shader(device, pipelines["perturb"][0], refs_bytes, CENTER, scale, WIDTH, HEIGHT, ITERATIONS, ref_len, *pipelines["perturb"][1:])
        bad = sum(1 for a, b in zip(p_iter, f64_iter) if a != b)
        print(f"{scale:>8.0e} {bad / len(f64_iter):>12.2%} {'0.00%':>10}")

    print(f"\nefficiency (best of 3):")
    print(f"{'scale':>8} {'perturb ms':>12}{'f64 ms':>10}{'speedup':>10}")
    for scale in (1e-8, 1e-12):
        times = {}
        for name, (pipeline, f32_out, uses_refs) in pipelines.items():
            best = float("inf")
            for _ in range(3):
                start = time.perf_counter()
                run_shader(device, pipeline, refs_bytes, CENTER, scale, WIDTH, HEIGHT, ITERATIONS, ref_len, f32_out, uses_refs)
                best = min(best, time.perf_counter() - start)
            times[name] = best * 1000
        print(f"{scale:>8.0e} {times['perturb']:>12.1f}{times['f64']:>10.1f}{times['f64'] / times['perturb']:>10.2f}x")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
