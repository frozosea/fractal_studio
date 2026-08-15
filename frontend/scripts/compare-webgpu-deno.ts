// WebGPU renderer parity harness (Deno).
//
// Alternative to compare-webgpu-render.mjs for environments where Chrome's
// Dawn cannot enumerate an adapter (e.g. missing SwiftShader component or
// blocked Vulkan): Deno exposes WebGPU over the system Vulkan loader, so on
// machines with a working Vulkan driver (or lavapipe) this runs the real
// WGSL shader path.
//
//   deno run --allow-all --unstable-webgpu scripts/compare-webgpu-deno.ts
//
// The same 7 cases as the Chrome harness: WGSL fp32 output is diffed against
// the C++-verified CPU fp64 core (local-render-core.renderLocalRgba).

import { renderWebGpuRgba } from "../src/lib/fractal/webgpu-renderer.ts";
import { renderLocalRgba, type LocalRenderSpec } from "../src/lib/fractal/local-render-core.ts";

const MAX_DELTA = 16;
const MAX_CHANGED_FRACTION = 0.02;

const CASES: Array<{ name: string; width: number; height: number; spec: LocalRenderSpec }> = [
  {
    name: "mandel_escape",
    width: 128, height: 96,
    spec: { centerRe: -0.75, centerIm: 0, scale: 3, iterations: 180, variant: "mandelbrot", metric: "escape", colorMap: "classic_cos", colorMode: "direct", cyclesPerOctave: 1, smooth: false, rotationDeg: 0, julia: false, juliaRe: 0, juliaIm: 0, bailout: 2, pairwiseCap: 64 },
  },
  {
    name: "ship_julia_smooth",
    width: 128, height: 96,
    spec: { centerRe: -0.64, centerIm: 0.03, scale: 2.4, iterations: 180, variant: "burning_ship", metric: "escape", colorMap: "viridis", colorMode: "direct", cyclesPerOctave: 1, smooth: true, rotationDeg: 19, julia: true, juliaRe: -0.8, juliaIm: 0.156, bailout: 2, pairwiseCap: 64 },
  },
  {
    name: "min_abs_hs_rainbow",
    width: 128, height: 96,
    spec: { centerRe: -0.75, centerIm: 0, scale: 3, iterations: 200, variant: "mandelbrot", metric: "min_abs", colorMap: "hs_rainbow", colorMode: "direct", cyclesPerOctave: 1, smooth: false, rotationDeg: 0, julia: false, juliaRe: 0, juliaIm: 0, bailout: 2, pairwiseCap: 64 },
  },
  {
    name: "max_abs_twilight_smooth",
    width: 128, height: 96,
    spec: { centerRe: -0.75, centerIm: 0, scale: 3, iterations: 200, variant: "tricorn", metric: "max_abs", colorMap: "twilight", colorMode: "direct", cyclesPerOctave: 1, smooth: true, rotationDeg: 0, julia: false, juliaRe: 0, juliaIm: 0, bailout: 2, pairwiseCap: 64 },
  },
  {
    name: "envelope_inferno_rotated",
    width: 128, height: 96,
    spec: { centerRe: -0.5, centerIm: 0, scale: 2.2, iterations: 220, variant: "heart", metric: "envelope", colorMap: "inferno", colorMode: "direct", cyclesPerOctave: 1, smooth: false, rotationDeg: 37, julia: false, juliaRe: 0, juliaIm: 0, bailout: 2, pairwiseCap: 64 },
  },
  {
    name: "perp_ship_spectral_julia",
    width: 128, height: 96,
    spec: { centerRe: -0.6, centerIm: 0.1, scale: 2, iterations: 220, variant: "perp_ship", metric: "escape", colorMap: "spectral1530", colorMode: "direct", cyclesPerOctave: 1, smooth: true, rotationDeg: -90, julia: true, juliaRe: -0.8, juliaIm: 0.156, bailout: 2, pairwiseCap: 64 },
  },
  {
    name: "deep_zoom_fp32_stress",
    width: 128, height: 96,
    spec: { centerRe: -0.7435, centerIm: 0.1314, scale: 0.0002, iterations: 600, variant: "mandelbrot", metric: "escape", colorMap: "classic_cos", colorMode: "direct", cyclesPerOctave: 1, smooth: false, rotationDeg: 0, julia: false, juliaRe: 0, juliaIm: 0, bailout: 2, pairwiseCap: 64 },
  },
];

const hasWebGpu = typeof navigator !== "undefined" && !!(navigator as { gpu?: unknown }).gpu;
if (!hasWebGpu) {
  console.error("navigator.gpu unavailable; run with --unstable-webgpu (Deno) or on a WebGPU-capable browser");
  Deno.exit(1);
}

let adapterAvailable = false;
try {
  adapterAvailable = !!(await navigator.gpu.requestAdapter());
} catch {
  adapterAvailable = false;
}
if (!adapterAvailable) {
  console.error("WebGPU adapter unavailable (requestAdapter returned null)");
  Deno.exit(1);
}

let failed = 0;
for (const entry of CASES) {
  const start = performance.now();
  let gpu: Uint8ClampedArray | null = null;
  let gpuError: string | null = null;
  try {
    gpu = await renderWebGpuRgba(entry.spec, entry.width, entry.height);
  } catch (error) {
    gpuError = String(error);
  }
  const gpuMs = Math.round(performance.now() - start);
  if (!gpu && !gpuError) gpuError = "renderWebGpuRgba returned null (ineligible or webgpu unavailable)";
  if (gpuError) {
    console.error(`webgpu/native parity: ${entry.name} ERROR: ${gpuError}`);
    failed += 1;
    continue;
  }
  const cpu = renderLocalRgba(entry.spec, entry.width, entry.height);
  let maxDelta = 0;
  let changed = 0;
  let mean = 0;
  for (let index = 0; index < gpu.length; index += 1) {
    const delta = Math.abs(gpu[index] - cpu[index]);
    maxDelta = Math.max(maxDelta, delta);
    if (delta > 1) changed += 1;
    mean += delta;
  }
  const changedFraction = changed / entry.width / entry.height;
  const ok = maxDelta <= MAX_DELTA && changedFraction <= MAX_CHANGED_FRACTION;
  if (!ok) {
    console.error(`webgpu/native parity: ${entry.name} FAIL maxDelta=${maxDelta} changed=${changed}/${entry.width * entry.height} (${(changedFraction * 100).toFixed(2)}%) mean=${(mean / gpu.length).toFixed(3)}`);
    failed += 1;
  } else {
    console.log(`webgpu/native parity: ${entry.name} passed (maxDelta=${maxDelta}, changed=${(changedFraction * 100).toFixed(2)}%, gpu=${gpuMs}ms)`);
  }
}
if (failed) {
  console.error(`webgpu/native parity: ${failed}/${CASES.length} cases failed`);
  Deno.exit(1);
}
console.log(`webgpu/native parity: ${CASES.length} cases passed (fp32 shader vs fp64 core)`);
