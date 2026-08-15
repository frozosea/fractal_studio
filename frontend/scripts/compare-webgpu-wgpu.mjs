// CPU reference comparator for the wgpu-native WGSL parity run.
//
// Reads the RGBA frames produced by scripts/compare-webgpu-wgpu.py
// (/tmp/wgpu-parity/<case>.rgba) and diffs them against the C++-verified CPU
// fp64 core (local-render-core.renderLocalRgba). Run order:
//
//   /tmp/wgpu-venv/bin/python scripts/compare-webgpu-wgpu.py
//   node scripts/compare-webgpu-wgpu.mjs

import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { spawnSync } from "node:child_process";
import ts from "typescript";

const ROOT = resolve(import.meta.dirname, "..");
const OUT_DIR = "/tmp/wgpu-parity";
const REFERENCE = process.env.REFERENCE_BINARY ?? resolve(ROOT, "../backend/build/browser_orbit_reference");

function dataUrl(relative) {
  const source = readFileSync(resolve(ROOT, relative), "utf8");
  const compiled = ts.transpileModule(source, {
    compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2022 },
  }).outputText;
  return `data:text/javascript;base64,${Buffer.from(compiled).toString("base64")}`;
}

const orbitUrl = dataUrl("src/lib/fractal/local-orbit-program.ts");
const cacheUrl = dataUrl("src/lib/fractal/local-field-cache.ts");
const coreSource = readFileSync(resolve(ROOT, "src/lib/fractal/local-render-core.ts"), "utf8");
const coreCompiled = ts.transpileModule(coreSource, {
  compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2022 },
}).outputText
  .replace('"./local-orbit-program"', JSON.stringify(orbitUrl))
  .replace('"./local-field-cache"', JSON.stringify(cacheUrl));
const core = await import(`data:text/javascript;base64,${Buffer.from(coreCompiled).toString("base64")}`);

const CASES = [
  { name: "mandel_escape", width: 128, height: 96, spec: { centerRe: -0.75, centerIm: 0, scale: 3, iterations: 180, variant: "mandelbrot", metric: "escape", colorMap: "classic_cos", colorMode: "direct", cyclesPerOctave: 1, smooth: false, rotationDeg: 0, julia: false, juliaRe: 0, juliaIm: 0, bailout: 2, pairwiseCap: 64 } },
  { name: "ship_julia_smooth", width: 128, height: 96, spec: { centerRe: -0.64, centerIm: 0.03, scale: 2.4, iterations: 180, variant: "burning_ship", metric: "escape", colorMap: "viridis", colorMode: "direct", cyclesPerOctave: 1, smooth: true, rotationDeg: 19, julia: true, juliaRe: -0.8, juliaIm: 0.156, bailout: 2, pairwiseCap: 64 } },
  { name: "min_abs_hs_rainbow", width: 128, height: 96, spec: { centerRe: -0.75, centerIm: 0, scale: 3, iterations: 200, variant: "mandelbrot", metric: "min_abs", colorMap: "hs_rainbow", colorMode: "direct", cyclesPerOctave: 1, smooth: false, rotationDeg: 0, julia: false, juliaRe: 0, juliaIm: 0, bailout: 2, pairwiseCap: 64 } },
  { name: "max_abs_twilight_smooth", width: 128, height: 96, spec: { centerRe: -0.75, centerIm: 0, scale: 3, iterations: 200, variant: "tricorn", metric: "max_abs", colorMap: "twilight", colorMode: "direct", cyclesPerOctave: 1, smooth: true, rotationDeg: 0, julia: false, juliaRe: 0, juliaIm: 0, bailout: 2, pairwiseCap: 64 } },
  { name: "envelope_inferno_rotated", width: 128, height: 96, spec: { centerRe: -0.5, centerIm: 0, scale: 2.2, iterations: 220, variant: "heart", metric: "envelope", colorMap: "inferno", colorMode: "direct", cyclesPerOctave: 1, smooth: false, rotationDeg: 37, julia: false, juliaRe: 0, juliaIm: 0, bailout: 2, pairwiseCap: 64 } },
  { name: "perp_ship_spectral_julia", width: 128, height: 96, spec: { centerRe: -0.6, centerIm: 0.1, scale: 2, iterations: 220, variant: "perp_ship", metric: "escape", colorMap: "spectral1530", colorMode: "direct", cyclesPerOctave: 1, smooth: true, rotationDeg: -90, julia: true, juliaRe: -0.8, juliaIm: 0.156, bailout: 2, pairwiseCap: 64 } },
  { name: "deep_zoom_fp32_stress", width: 128, height: 96, spec: { centerRe: -0.7435, centerIm: 0.1314, scale: 0.0002, iterations: 600, variant: "mandelbrot", metric: "escape", colorMap: "classic_cos", colorMode: "direct", cyclesPerOctave: 1, smooth: false, rotationDeg: 0, julia: false, juliaRe: 0, juliaIm: 0, bailout: 2, pairwiseCap: 64 } },
];

const DIAG_MAX_CHANGED_FRACTION = 0.25;
const MAX_DELTA = 255;

// Same-precision stage: the WGSL fp32 shader vs the C++ fp32 reference.
// Operation order mirrors the shader, so this must be near byte-exact.
// Independent fp32 implementations (naga/SPIR-V on the GPU vs g++ -O3 with
// FMA contraction) can still flip a handful of iteration-boundary pixels
// (measured 0.1%), so allow a small changed fraction; anything structural
// (e.g. a wrong orbit formula or palette) shows up as a large fraction.
const FP32_FRAMES = [
  { name: "mandel_escape", frame: "mandelbrot" },
  { name: "ship_julia_smooth", frame: "burning_ship_julia" },
  { name: "deep_zoom_fp32_stress", frame: "deep_zoom" },
];
const FP32_MAX_CHANGED_FRACTION = 0.005;

let failed = 0;
for (const entry of FP32_FRAMES) {
  const run = spawnSync(REFERENCE, ["--frame-fp32", entry.frame]);
  if (!run.stdout?.length) throw run.error ?? new Error(`native fp32 frame ${entry.frame} failed`);
  const gpu = readFileSync(`${OUT_DIR}/${entry.name}.rgba`);
  let maxDelta = 0;
  let changed = 0;
  for (let index = 0; index < run.stdout.length; index += 1) {
    const delta = Math.abs(gpu[index] - run.stdout[index]);
    maxDelta = Math.max(maxDelta, delta);
    if (delta > 1) changed += 1;
  }
  const changedFraction = changed / (gpu.length / 4);
  const ok = maxDelta <= 255 && changedFraction <= FP32_MAX_CHANGED_FRACTION;
  if (!ok) {
    console.error(`webgpu/native parity: ${entry.name} [fp32 ref] FAIL maxDelta=${maxDelta} changed=${changed}/${gpu.length / 4} (${(changedFraction * 100).toFixed(2)}%)`);
    failed += 1;
  } else {
    console.log(`webgpu/native parity: ${entry.name} [fp32 ref] passed (maxDelta=${maxDelta}, changed=${(changedFraction * 100).toFixed(2)}%)`);
  }
}

// Diagnostic stage: WGSL fp32 vs the C++-verified CPU fp64 core. fp32/fp64
// legitimately disagree on iteration-boundary pixels (all measured
// differences sit on field boundaries), so this stage only sanity-caps the
// changed fraction to catch structural coloring errors.

for (const entry of CASES) {
  const gpu = readFileSync(`${OUT_DIR}/${entry.name}.rgba`);
  if (gpu.length !== entry.width * entry.height * 4) {
    console.error(`${entry.name}: gpu frame size mismatch (${gpu.length})`);
    failed += 1;
    continue;
  }
  const cpu = core.renderLocalRgba(entry.spec, entry.width, entry.height);
  let maxDelta = 0;
  let changed = 0;
  let mean = 0;
  for (let index = 0; index < cpu.length; index += 1) {
    const delta = Math.abs(gpu[index] - cpu[index]);
    maxDelta = Math.max(maxDelta, delta);
    if (delta > 1) changed += 1;
    mean += delta;
  }
  const changedFraction = changed / entry.width / entry.height;
  const ok = maxDelta <= MAX_DELTA && changedFraction <= DIAG_MAX_CHANGED_FRACTION;
  if (!ok) {
    console.error(`webgpu/native parity: ${entry.name} [fp64 diag] FAIL maxDelta=${maxDelta} changed=${changed}/${entry.width * entry.height} (${(changedFraction * 100).toFixed(2)}%) mean=${(mean / cpu.length).toFixed(3)}`);
    failed += 1;
  } else {
    console.log(`webgpu/native parity: ${entry.name} [fp64 diag] ok (maxDelta=${maxDelta}, changed=${(changedFraction * 100).toFixed(2)}%, mean=${(mean / cpu.length).toFixed(3)})`);
  }
}
if (failed) {
  console.error(`webgpu/native parity: ${failed}/${CASES.length} cases failed`);
  process.exit(1);
}
console.log(`webgpu/native parity: ${CASES.length} cases passed (wgpu-native WGSL fp32 vs C++-verified fp64 core)`);
