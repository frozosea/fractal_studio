// WebGPU renderer parity harness.
//
// Compares the WebGPU compute preview renderer (fp32, WGSL) against the
// C++-verified CPU TypeScript core (fp64) for the same render specs, pixel by
// pixel. The CPU core is itself validated against the native backend by
// compare-local-render.mjs, so this closes the loop on the shader path.
//
// WebGPU is not exposed to Playwright-launched Chrome in this environment, so
// the harness launches Chrome itself with the SwiftShader flags that unlock
// headless WebGPU, then attaches over CDP (connectOverCDP). Override the
// binary with CHROME_BIN, e.g.:
//
//   CHROME_BIN=/usr/bin/google-chrome npm run test:webgpu-parity
//
// The GPU adapter is software (SwiftShader): slow, but deterministic enough
// for byte-level diffing with a small boundary allowance.

import { spawn } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import ts from "typescript";
import { chromium } from "@playwright/test";

const ROOT = resolve(import.meta.dirname, "..");

// ---------------------------------------------------------------------------
// 1. Transpile the TypeScript renderer modules into inline classic scripts.
//    local-render-core imports "./local-orbit-program" and "./local-field-cache";
//    webgpu-renderer is type-only dependent on local-render-core, so it is
//    self-contained after transpilation.
// ---------------------------------------------------------------------------

function transpile(relative) {
  const source = readFileSync(resolve(ROOT, relative), "utf8");
  return ts.transpileModule(source, {
    compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022 },
  }).outputText;
}

const orbitCode = transpile("src/lib/fractal/local-orbit-program.ts");
const cacheCode = transpile("src/lib/fractal/local-field-cache.ts");
const coreCode = transpile("src/lib/fractal/local-render-core.ts")
  .replace('require("./local-orbit-program")', "orbitExports")
  .replace('require("./local-field-cache")', "cacheExports");
const webgpuCode = transpile("src/lib/fractal/webgpu-renderer.ts");

// ---------------------------------------------------------------------------
// 2. Render cases. CPU (fp64) vs GPU (fp32) differences concentrate on
//    iteration boundaries; each case reports per-channel deltas.
// ---------------------------------------------------------------------------

const CASES = [
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

// fp32 vs fp64: allow small per-channel deltas on boundary pixels and a small
// fraction of changed pixels overall. Escaped interior pixels must match byte
// for byte.
const MAX_DELTA = 16;
const MAX_CHANGED_FRACTION = 0.02;

const harness = `<!doctype html><html><body><pre id="out">running</pre>
<script>
var module = { exports: {} };
var exports = module.exports;
var orbitExports = null;
var cacheExports = null;
var require = function (name) {
  if (name === "./local-orbit-program") return orbitExports;
  if (name === "./local-field-cache") return cacheExports;
  throw new Error("unknown require " + name);
};
</script>
<script>${orbitCode}</script>
<script>orbitExports = module.exports; module.exports = {}; exports = module.exports;</script>
<script>${cacheCode}</script>
<script>cacheExports = module.exports; module.exports = {}; exports = module.exports;</script>
<script>${coreCode}</script>
<script>var coreExports = module.exports; module.exports = {}; exports = module.exports;</script>
<script>${webgpuCode}</script>
<script>var webgpuExports = module.exports;</script>
<script>
window.__parity = (async () => {
  let adapterAvailable = false;
  try {
    if (navigator.gpu) adapterAvailable = !!(await navigator.gpu.requestAdapter());
  } catch (error) {
    adapterAvailable = false;
  }
  const cases = ${JSON.stringify(CASES)};
  const results = [];
  for (const entry of cases) {
    const start = performance.now();
    let gpu = null;
    let gpuError = null;
    try {
      gpu = await webgpuExports.renderWebGpuRgba(entry.spec, entry.width, entry.height);
    } catch (error) {
      gpuError = String(error && error.stack ? error.stack : error);
    }
    const gpuMs = Math.round(performance.now() - start);
    if (!gpu && !gpuError) gpuError = "renderWebGpuRgba returned null (webgpu_unavailable or ineligible)";
    let stats = null;
    if (gpu) {
      const cpu = coreExports.renderLocalRgba(entry.spec, entry.width, entry.height);
      const histogram = new Array(256).fill(0);
      let maxDelta = 0;
      let changed = 0;
      let mean = 0;
      const pixels = entry.width * entry.height;
      for (let index = 0; index < gpu.length; index += 1) {
        const delta = Math.abs(gpu[index] - cpu[index]);
        histogram[Math.min(255, delta | 0)] += 1;
        maxDelta = Math.max(maxDelta, delta);
        if (delta > 1) changed += 1;
        mean += delta;
      }
      stats = { maxDelta, changed, changedFraction: changed / (gpu.length / 4), meanDelta: mean / gpu.length, histogram };
    }
    results.push({ name: entry.name, width: entry.width, height: entry.height, gpuMs, gpuError, stats });
  }
  return { webgpu: !!navigator.gpu, adapterAvailable, results };
})();
</script></body></html>`;

// ---------------------------------------------------------------------------
// 3. Launch Chrome with headless WebGPU (SwiftShader) and attach over CDP.
// ---------------------------------------------------------------------------

const port = 9300 + Math.floor(Math.random() * 500);
const profile = mkdtempSync(join(tmpdir(), "wg-parity-"));
const chromeBin = process.env.CHROME_BIN ?? "/usr/bin/google-chrome";
const htmlPath = join(tmpdir(), "wg-parity.html");
writeFileSync(htmlPath, harness);

const chrome = spawn(chromeBin, [
  "--headless=new",
  "--no-sandbox",
  "--disable-dev-shm-usage",
  "--enable-unsafe-swiftshader",
  "--use-webgpu-adapter=swiftshader",
  `--remote-debugging-port=${port}`,
  `--user-data-dir=${profile}`,
  "about:blank",
], { stdio: "ignore" });

async function waitForEndpoint(url, timeoutMs = 20000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    try {
      const response = await fetch(url);
      if (response.ok) return;
    } catch { /* not up yet */ }
    await new Promise((resolveDelay) => setTimeout(resolveDelay, 400));
  }
  throw new Error(`Chrome DevTools endpoint ${url} did not come up`);
}

let browser;
try {
  await waitForEndpoint(`http://127.0.0.1:${port}/json/version`);
  browser = await chromium.connectOverCDP(`http://127.0.0.1:${port}`);
  const page = await browser.newPage();
  await page.goto(`file://${htmlPath}`);
  const result = await page.evaluate(() => window.__parity, { timeout: 240000 });
  await browser.close();
  browser = undefined;
  chrome.kill();

  if (!result || !Array.isArray(result.results)) throw new Error("harness returned no results");
  if (!result.webgpu) throw new Error("WebGPU unavailable in the launched browser (navigator.gpu missing)");
  if (!result.adapterAvailable) throw new Error("WebGPU adapter unavailable (requestAdapter returned null); SwiftShader WebGPU did not initialize in this Chrome/environment");

  let failed = 0;
  for (const entry of result.results) {
    if (entry.gpuError) {
      console.error(`webgpu/native parity: ${entry.name} ERROR: ${entry.gpuError.split("\\n")[0]}`);
      failed += 1;
      continue;
    }
    const stats = entry.stats;
    const changedFraction = stats.changed / (entry.width * entry.height);
    const ok = stats.maxDelta <= MAX_DELTA && changedFraction <= MAX_CHANGED_FRACTION;
    if (!ok) {
      console.error(`webgpu/native parity: ${entry.name} FAIL maxDelta=${stats.maxDelta} changed=${stats.changed}/${entry.width * entry.height} (${(changedFraction * 100).toFixed(2)}%) mean=${stats.meanDelta.toFixed(3)}`);
      failed += 1;
    } else {
      console.log(`webgpu/native parity: ${entry.name} passed (maxDelta=${stats.maxDelta}, changed=${(changedFraction * 100).toFixed(2)}%, gpu=${entry.gpuMs}ms)`);
    }
  }
  if (failed) {
    console.error(`webgpu/native parity: ${failed}/${result.results.length} cases failed`);
    process.exit(1);
  }
  console.log(`webgpu/native parity: ${result.results.length} cases passed (fp32 shader vs fp64 core)`);
} finally {
  if (browser) await browser.close().catch(() => {});
  chrome.kill();
  rmSync(profile, { recursive: true, force: true });
  rmSync(htmlPath, { force: true });
}
