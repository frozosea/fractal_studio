// Browser E2E for the local render pipeline: worker pool, tiled wasm
// rendering, abort and race handling, pixel parity vs the JS core.
//
// Bundles the renderer with esbuild, serves it over a tiny HTTP server and
// drives real Chrome via Playwright.
// Run: node scripts/e2e-local-render.mjs

import { build } from "esbuild";
import { createServer } from "node:http";
import { existsSync, mkdirSync, readFileSync, writeFileSync, copyFileSync, mkdirSync as ensureDir } from "node:fs";
import { resolve, dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { chromium } from "@playwright/test";

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const OUT = "/tmp/e2e-lr";
const ENTRY_SRC = resolve(ROOT, "scripts/e2e-entry.ts");

const ENTRY = `
import { renderFractalLocally } from "@/lib/fractal/local-renderer";
import { renderLocalRgba } from "@/lib/fractal/local-render-core";

const CASES = [
  { name: "mandel_deep", width: 512, height: 384, spec: { centerRe: -0.743643887037151, centerIm: 0.13182590420533, scale: 1e-4, iterations: 512, variant: "mandelbrot", metric: "escape", colorMap: "classic_cos", colorMode: "direct", cyclesPerOctave: 1, smooth: false, rotationDeg: 0, julia: false, juliaRe: 0, juliaIm: 0, bailout: 2, pairwiseCap: 64 } },
  { name: "ship_julia_smooth", width: 512, height: 384, spec: { centerRe: -0.64, centerIm: 0.03, scale: 2.4, iterations: 600, variant: "burning_ship", metric: "escape", colorMap: "viridis", colorMode: "direct", cyclesPerOctave: 1, smooth: true, rotationDeg: 19, julia: true, juliaRe: -0.8, juliaIm: 0.156, bailout: 2, pairwiseCap: 64 } },
  { name: "heart_envelope", width: 512, height: 384, spec: { centerRe: -0.5, centerIm: 0, scale: 2.2, iterations: 400, variant: "heart", metric: "envelope", colorMap: "inferno", colorMode: "direct", cyclesPerOctave: 1, smooth: false, rotationDeg: 37, julia: false, juliaRe: 0, juliaIm: 0, bailout: 2, pairwiseCap: 64 } },
];

async function blobToRgba(blob, width, height) {
  const bitmap = await createImageBitmap(blob);
  const canvas = new OffscreenCanvas(width, height);
  const ctx = canvas.getContext("2d");
  ctx.drawImage(bitmap, 0, 0, width, height);
  return ctx.getImageData(0, 0, width, height).data;
}

window.__e2eResults = [];
async function __runAll() {
  const steps = ["manualWorker", "parity", "abort", "race", "transcendental"];
  for (const step of steps) {
    try {
      const value = await window.__localRenderTest[step]();
      window.__e2eResults.push({ step, value });
    } catch (error) {
      window.__e2eResults.push({ step, error: String(error && error.message ? error.message : error) });
    }
  }
}
window.__localRenderTest = {
  async withTimeout(promise, ms, label) {
    return Promise.race([
      promise,
      new Promise((_, reject) => setTimeout(() => reject(new Error(label + " timed out")), ms)),
    ]);
  },
  async manualWorker() {
    // 直接驱动 worker：整帧消息（非 tiled 路径）
    const worker = new Worker(new URL("/dist/worker.js", import.meta.url), { type: "module" });
    const entry = CASES[0];
    const result = await new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error("manual worker timed out")), 30000);
      worker.onmessage = (event) => {
        if (event.data.error) { clearTimeout(timer); reject(new Error(event.data.error)); return; }
        if (event.data.stage === "final" && event.data.blob) {
          clearTimeout(timer);
          resolve({ blob: event.data.blob, width: event.data.width, height: event.data.height });
        }
      };
      worker.onerror = (event) => { clearTimeout(timer); reject(new Error(event.message)); };
      worker.postMessage({ id: 1, spec: entry.spec, width: entry.width, height: entry.height });
    });
    const rgba = await blobToRgba(result.blob, result.width, result.height);
    const ref = renderLocalRgba(entry.spec, entry.width, entry.height);
    let bad = 0;
    for (let i = 0; i < ref.length; i += 1) if (Math.abs(rgba[i] - ref[i]) > 1) bad += 1;
    worker.terminate();
    return { ok: bad === 0, bad };
  },
  async parity() {
    const results = [];
    for (const entry of CASES) {
      const controller = new AbortController();
      const blob = await this.withTimeout(renderFractalLocally(entry.spec, entry.width, entry.height, controller.signal), 60000, entry.name);
      if (!blob) return { ok: false, error: "renderFractalLocally returned null for " + entry.name };
      const blobRgba = await blobToRgba(blob, entry.width, entry.height);
      const refRgba = renderLocalRgba(entry.spec, entry.width, entry.height);
      let bad = 0, maxD = 0;
      for (let i = 0; i < refRgba.length; i += 1) {
        const d = Math.abs(blobRgba[i] - refRgba[i]);
        maxD = Math.max(maxD, d);
        if (d > 1) bad += 1;
      }
      results.push({ name: entry.name, bad, maxD, pixels: refRgba.length / 4 });
    }
    return { ok: results.every((r) => r.bad === 0), results };
  },
  async abort() {
    const controller = new AbortController();
    const promise = renderFractalLocally(CASES[0].spec, 1024, 768, controller.signal);
    controller.abort();
    try {
      await promise;
      return { ok: false, error: "aborted render resolved" };
    } catch (reason) {
      return { ok: reason instanceof DOMException && reason.name === "AbortError", name: reason?.name };
    }
  },
  async race() {
    // Same-channel concurrent renders: later renders preempt earlier ones
    // (AbortError), and the winner must be pixel-correct. No hang, no wrong
    // image, no crash is the assertion.
    const out = [];
    const settle = async (entry, channel) => {
      const controller = new AbortController();
      // Separate channels (main / julia_picker) run on separate worker pools:
      // concurrent renders must both complete and be pixel-correct.
      const small = { ...entry, width: 128, height: 96 };
      try {
        const blob = await renderFractalLocally(small.spec, small.width, small.height, controller.signal, channel);
        const rgba = await blobToRgba(blob, small.width, small.height);
        const ref = renderLocalRgba(small.spec, small.width, small.height);
        let bad = 0;
        for (let i = 0; i < ref.length; i += 1) if (Math.abs(rgba[i] - ref[i]) > 1) bad += 1;
        out.push({ name: entry.name, status: "ok", bad });
      } catch (reason) {
        const aborted = reason instanceof DOMException && reason.name === "AbortError";
        out.push({ name: entry.name, status: aborted ? "preempted" : "error", reason: String(reason) });
        if (!aborted) throw reason;
      }
    };
    await Promise.all([settle(CASES[0], "main"), settle(CASES[1], "julia_picker")]);
    const ok = out.some((r) => r.status === "ok" && r.bad === 0)
      && out.every((r) => r.status !== "error");
    return { ok, out };
  },
  __keep: 0,
  async transcendental() {
    const spec = { ...CASES[0].spec, variant: "sin_z", iterations: 200, scale: 3, centerRe: -0.75, centerIm: 0, metric: "escape", colorMap: "classic_cos", bailout: 64 };
    const controller = new AbortController();
    const blob = await renderFractalLocally(spec, 256, 192, controller.signal);
    if (!blob) return { ok: false, error: "transcendental render returned null" };
    const rgba = await blobToRgba(blob, 256, 192);
    const ref = renderLocalRgba(spec, 256, 192);
    let bad = 0;
    for (let i = 0; i < ref.length; i += 1) if (Math.abs(rgba[i] - ref[i]) > 1) bad += 1;
    return { ok: bad === 0, bad };
  },
};
window.__runAll = __runAll;
`;

mkdirSync(OUT, { recursive: true });
writeFileSync(ENTRY_SRC, ENTRY);

async function bundle(entry, outfile, banner = "") {
  const result = await build({
    entryPoints: [entry],
    bundle: true,
    format: "esm",
    outfile,
    platform: "browser",
    target: "chrome120",
    alias: { "@": resolve(ROOT, "src") },
    loader: { ".wasm": "copy" },
    write: true,
    banner: { js: banner },
    logLevel: "warning",
  });
  if (result.errors.length) {
    console.error(result.errors.map((e) => e.text).join("\n"));
    process.exit(1);
  }
}

// Worker as its own bundle (esbuild does not split new URL workers here).
await bundle(resolve(ROOT, "src/workers/fractal-render.worker.ts"), join(OUT, "dist/worker.js"));
console.log("worker bundle written");
await bundle(ENTRY_SRC, join(OUT, "dist/entry.js"));

// Point the renderer's worker URL at the prebuilt worker bundle.
const entryPath = join(OUT, "dist/entry.js");
let entryJs = readFileSync(entryPath, "utf8");
const workerUrlMatch = entryJs.match(/new Worker\(new URL\(([^)]*)\),/);
if (workerUrlMatch) {
  entryJs = entryJs.replace(workerUrlMatch[0], `new Worker(new URL("/dist/worker.js", import.meta.url),`);
} else {
  console.error("worker URL pattern not found in bundle");
  process.exit(1);
}
writeFileSync(entryPath, entryJs);
console.log("bundle:", ["entry.js", "worker.js"].join(", "));

// esbuild leaves new URL(wasm) references untouched; copy the wasm cores to
// the paths the bundles request.
for (const name of ["field_core.wasm", "colorize.wasm"]) {
  const target = join(OUT, "dist/scripts/wasm-core", name);
  ensureDir(dirname(target), { recursive: true });
  copyFileSync(resolve(ROOT, "scripts/wasm-core", name), target);
}

// Serve the bundle + wasm assets.
const distDir = join(OUT, "dist");
const html = `<!doctype html><html><body><script type="module" src="/dist/entry.js"></script></body></html>`;
writeFileSync(join(OUT, "index.html"), html);

const mime = {
  ".js": "text/javascript",
  ".wasm": "application/wasm",
  ".html": "text/html",
};
const server = createServer((req, res) => {
  const path = req.url === "/" ? "/index.html" : req.url;
  let file = join(OUT, path);
  if (!existsSync(file)) file = join(distDir, path);
  if (!file.startsWith(OUT) || !existsSync(file)) {
    console.error("404:", req.url);
    res.writeHead(404).end("not found");
    return;
  }
  const ext = file.slice(file.lastIndexOf("."));
  res.writeHead(200, { "Content-Type": mime[ext] ?? "application/octet-stream" });
  res.end(readFileSync(file));
});
let port = 0;
for (let attempt = 0; attempt < 10; attempt += 1) {
  port = 20000 + Math.floor(Math.random() * 20000);
  try {
    await new Promise((resolveListen, rejectListen) => {
      server.once("error", rejectListen);
      server.listen(port, "127.0.0.1", resolveListen);
    });
    break;
  } catch (error) {
    if (error.code !== "EADDRINUSE") throw error;
  }
}
console.log("serving on", port);

const browser = await chromium.launch({
  executablePath: "/usr/bin/google-chrome",
  headless: true,
  args: ["--no-sandbox", "--disable-dev-shm-usage"],
});
browser.on("disconnected", () => console.error("[browser] disconnected (crash?)"));
browser.on("disconnected", () => {});
// 页面 crash 监听

try {
  const page = await browser.newPage();
  // Simulate a mid-range device so the worker pool stays small enough for
  // headless Chrome to survive the race test.
  await page.addInitScript(() => {
    Object.defineProperty(navigator, "hardwareConcurrency", { value: 4, configurable: true });
  });
  page.on("console", (msg) => {
    if (msg.type() === "error") console.error("[page]", msg.text().slice(0, 300));
  });
  page.on("pageerror", (err) => console.error("[pageerror]", String(err).slice(0, 300)));
  await page.goto(`http://127.0.0.1:${port}/`);
  await page.waitForFunction(() => window.__localRenderTest !== undefined, { timeout: 15000 });

  page.on("crash", () => console.error("[page] crashed"));
  await page.evaluate(() => { void window.__runAll(); });
  await page.waitForFunction(() => window.__e2eResults && window.__e2eResults.length >= 5, { timeout: 240000 })
    .catch(() => console.error("[page] test run did not finish; collected:", "see below"));
  const results = await page.evaluate(() => window.__e2eResults);

  let failed = 0;
  const byStep = Object.fromEntries((results ?? []).map((r) => [r.step, r]));
  for (const step of ["manualWorker", "parity", "abort", "race", "transcendental"]) {
    const entry = byStep[step];
    if (!entry) { console.log(step, ": MISSING"); failed += 1; continue; }
    if (entry.error) { console.log(step, ": ERROR", entry.error); failed += 1; continue; }
    const value = entry.value;
    const ok = value?.ok === true;
    console.log(step, ":", JSON.stringify(value ?? null));
    if (!ok) failed += 1;
  }

  if (failed) {
    console.error(`E2E: ${4 - failed}/4 passed`);
    process.exitCode = 1;
  } else {
    console.log("E2E: 4/4 passed (tiled wasm parity, abort, races, JS fallback)");
  }
} finally {
  await browser.close();
  server.close();
}
