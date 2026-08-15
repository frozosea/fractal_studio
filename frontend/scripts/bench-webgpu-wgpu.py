import math, re, struct, time, wgpu

#!/usr/bin/env python3
"""WebGPU full-renderer benchmark (wgpu-native).

Times the exact FRACTAL_SHADER from webgpu-renderer.ts (full fp32 preview
pipeline including coloring) on whatever Vulkan adapter wgpu selects, so the
same script measures dGPU, iGPU and (when supported) software adapters.

  # NVIDIA dGPU
  /tmp/wgpu-venv/bin/python scripts/bench-webgpu-wgpu.py
  # Intel iGPU (needs renderD128 access)
  VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/intel_icd.json WGPU_BACKEND=vulkan \
    /tmp/wgpu-venv/bin/python scripts/bench-webgpu-wgpu.py
"""

import math, re, struct, time, wgpu

src = open("src/lib/fractal/webgpu-renderer.ts").read()
wgsl = re.search(r"FRACTAL_SHADER\s*=\s*/\*\s*wgsl\s*\*/\s*`(.*?)`", src, re.S).group(1)

adapter = wgpu.gpu.request_adapter_sync(power_preference="high-performance")
print("adapter:", adapter.info["device"], "/", adapter.info["backend_type"], "/", adapter.info["adapter_type"])
device = adapter.request_device_sync()
shader = device.create_shader_module(code=wgsl)
pipeline = device.create_compute_pipeline(layout="auto", compute={"module": shader, "entry_point": "main"})

def uniform(spec, w, h):
    angle = spec["rotationDeg"] * math.pi / 180
    floats = [spec["centerRe"], spec["centerIm"], spec["scale"], w/h,
              math.cos(angle), math.sin(angle), spec["bailout"], spec["bailout"]**2,
              spec["juliaRe"], spec["juliaIm"], 0.0, 0.0]
    u32s = [w, h, spec["iterations"], 0, 0, 0, 1 if spec["smooth"] else 0, 1 if spec["julia"] else 0]
    return struct.pack("<12f", *floats) + struct.pack("<8I", *u32s)

def render(spec, w, h):
    params = device.create_buffer(size=80, usage=wgpu.BufferUsage.UNIFORM | wgpu.BufferUsage.COPY_DST)
    pixels = device.create_buffer(size=w*h*4, usage=wgpu.BufferUsage.STORAGE | wgpu.BufferUsage.COPY_SRC)
    read = device.create_buffer(size=w*h*4, usage=wgpu.BufferUsage.COPY_DST | wgpu.BufferUsage.MAP_READ)
    device.queue.write_buffer(params, 0, uniform(spec, w, h))
    bg = device.create_bind_group(layout=pipeline.get_bind_group_layout(0), entries=[
        {"binding": 0, "resource": params}, {"binding": 1, "resource": pixels}])
    enc = device.create_command_encoder()
    p = enc.begin_compute_pass(); p.set_pipeline(pipeline); p.set_bind_group(0, bg)
    p.dispatch_workgroups(math.ceil(w/8), math.ceil(h/8)); p.end()
    enc.copy_buffer_to_buffer(pixels, 0, read, 0, w*h*4)
    device.queue.submit([enc.finish()])
    read.map_sync(wgpu.MapMode.READ)
    bytes(read.read_mapped()); read.unmap()

CASES = [
    ("preview_512x384x512", 512, 384, {"centerRe": -0.743643887037151, "centerIm": 0.13182590420533, "scale": 1e-4, "iterations": 512, "rotationDeg": 0, "bailout": 2, "juliaRe": 0, "juliaIm": 0, "smooth": False, "julia": False}),
    ("preview_512x384x1500", 512, 384, {"centerRe": -0.743643887037151, "centerIm": 0.13182590420533, "scale": 1e-4, "iterations": 1500, "rotationDeg": 0, "bailout": 2, "juliaRe": 0, "juliaIm": 0, "smooth": False, "julia": False}),
    ("full_1024x768x512", 1024, 768, {"centerRe": -0.743643887037151, "centerIm": 0.13182590420533, "scale": 1e-4, "iterations": 512, "rotationDeg": 0, "bailout": 2, "juliaRe": 0, "juliaIm": 0, "smooth": False, "julia": False}),
    ("full_1024x768x1500", 1024, 768, {"centerRe": -0.743643887037151, "centerIm": 0.13182590420533, "scale": 1e-4, "iterations": 1500, "rotationDeg": 0, "bailout": 2, "juliaRe": 0, "juliaIm": 0, "smooth": False, "julia": False}),
]
print("WebGPU fp32 full renderer:")
print(f"{'case'.ljust(22)}{'ms'.rjust(9)}{'Mpix/s'.rjust(10)}")
for name, w, h, spec in CASES:
    render(spec, 64, 48)  # warmup
    best = float("inf")
    for _ in range(3):
        start = time.perf_counter()
        render(spec, w, h)
        best = min(best, (time.perf_counter() - start) * 1000)
    mpix = w * h / best / 1000
    print(f"{name.ljust(22)}{best:.1f}".rjust(9).rjust(32 - 22) if False else f"{name.ljust(22)}{best:>9.1f}{mpix:>10.2f}")
