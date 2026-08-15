// wasm SIMD iteration-core benchmark vs JS fp64 worker core.
// Requires scripts/wasm-core/field_iters.wasm built with:
//   clang --target=wasm32 -O3 -msimd128 -nostdlib -Wl,--no-entry \
//     -Wl,--export=field_iters_simd -Wl,--export=field_iters_scalar \
//     -o scripts/wasm-core/field_iters.wasm scripts/wasm-core/field_iters.cpp
// Run: node scripts/bench-wasm-core.mjs

import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { Worker } from "node:worker_threads";
import ts from "typescript";

const ROOT = resolve(import.meta.dirname, "..");

// ---- JS reference: the real worker core (fp64) ----------------------------
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

// ---- wasm ---------------------------------------------------------------
const wasmPath = resolve(ROOT, "scripts/wasm-core/field_iters.wasm");
const wasmBytes = readFileSync(wasmPath);
const { instance } = await WebAssembly.instantiate(wasmBytes, {});
const { memory, field_iters_simd, field_iters_scalar } = instance.exports;

function wasmField(fn, width, height, iterations, y0 = 0, tileHeight = 0) {
  const centerRe = -0.743643887037151, centerIm = 0.13182590420533, scale = 1e-4;
  const outPtr = 0;
  const rows = tileHeight || height;
  fn(centerRe, centerIm, scale, iterations, 2.0, width, height, y0, rows, outPtr);
  return new Uint32Array(memory.buffer, 0, rows * width);
}

const CASES = [
  { name: "preview_512x384x512", width: 512, height: 384, iterations: 512 },
  { name: "preview_512x384x1500", width: 512, height: 384, iterations: 1500 },
  { name: "full_1024x768x512", width: 1024, height: 768, iterations: 512 },
  { name: "full_1024x768x1500", width: 1024, height: 768, iterations: 1500 },
];

const baseSpec = {
  centerRe: -0.743643887037151, centerIm: 0.13182590420533, scale: 1e-4,
  variant: "mandelbrot", metric: "escape", colorMap: "classic_cos",
  colorMode: "direct", cyclesPerOctave: 1, smooth: false, rotationDeg: 0,
  julia: false, juliaRe: 0, juliaIm: 0, bailout: 2, pairwiseCap: 64,
};

function time(fn, runs = 3) {
  let best = Infinity;
  for (let i = 0; i < runs; i += 1) {
    const start = performance.now();
    fn();
    best = Math.min(best, performance.now() - start);
  }
  return best;
}

// ---- multi-worker (tiled) wasm timing via worker_threads ------------------
const workerSource = `
const { parentPort, workerData } = require("node:worker_threads");
const { readFileSync } = require("node:fs");
(async () => {
  const wasm = readFileSync(workerData.wasmPath);
  const { instance } = await WebAssembly.instantiate(wasm, {});
  const fn = instance.exports[workerData.fn];
  const memory = instance.exports.memory;
  parentPort.postMessage({ ready: true });
  parentPort.on("message", (msg) => {
    const start = performance.now();
    const outPtr = 0;
    fn(msg.centerRe, msg.centerIm, msg.scale, msg.iterations, 2.0, msg.width, msg.height, msg.y0, msg.rows, outPtr);
    parentPort.postMessage({ ms: performance.now() - start });
  });
})();`;

function multiWorker(workers, width, height, iterations) {
  return new Promise((resolvePromise) => {
    const threads = [];
    let done = 0;
    let best = Infinity;
    for (let w = 0; w < workers; w += 1) {
      const worker = new Worker(workerSource, { eval: true, workerData: { wasmPath, fn: "field_iters_simd" } });
      threads.push(worker);
    }
    let readyCount = 0;
    const startAll = () => {
      const started = performance.now();
      const rowsPer = Math.ceil(height / workers);
      for (let w = 0; w < workers; w += 1) {
        const y0 = w * rowsPer;
        const rows = Math.min(rowsPer, height - y0);
        threads[w].postMessage({ width, height, y0, rows, iterations, centerRe: -0.743643887037151, centerIm: 0.13182590420533, scale: 1e-4 });
      }
      const check = (ms) => {
        best = Math.max(best, ms); // wall time = slowest worker
        if (++done === workers) {
          const wall = performance.now() - started;
          resolvePromise({ wall, slowest: best });
          threads.forEach((t) => t.terminate());
        }
      };
      threads.forEach((t) => t.on("message", (m) => { if (m.ms !== undefined) check(m.ms); }));
    };
    threads.forEach((t) => t.on("message", (m) => { if (m.ready) { readyCount += 1; if (readyCount === workers) startAll(); } }));
  });
}

console.log("mandelbrot escape field, center -0.743643887037151+0.13182590420533i, scale 1e-4:");
console.log(`${"case".padEnd(22)}${"JS(ms)".padStart(8)}${"wasm-scalar(ms)".padStart(15)}${"wasm-simd(ms)".padStart(14)}${"simd/js".padStart(9)}${"simd/scalar".padStart(12)}`);
for (const entry of CASES) {
  const js = time(() => { core.renderLocalRgba({ ...baseSpec, iterations: entry.iterations }, entry.width, entry.height); });
  const was = time(() => { wasmField(field_iters_scalar, entry.width, entry.height, entry.iterations); });
  const wsi = time(() => { wasmField(field_iters_simd, entry.width, entry.height, entry.iterations); });
  console.log(`${entry.name.padEnd(22)}${js.toFixed(0).padStart(8)}${was.toFixed(1).padStart(15)}${wsi.toFixed(1).padStart(14)}${(wsi / js).toFixed(2).padStart(9)}${(wsi / was).toFixed(2).padStart(12)}`);
}

// 多 worker 分块（scalar wasm，模拟多核）
const hc = 8;
console.log(`\nmulti-worker tiled (${hc} threads, wasm SIMD, one tile each):`);
for (const entry of CASES.slice(0, 2)) {
  const r = await multiWorker(hc, entry.width, entry.height, entry.iterations);
  const mpix = entry.width * entry.height / r.wall / 1000;
  console.log(`${entry.name.padEnd(22)}${r.wall.toFixed(1).padStart(8)} ms  (${mpix.toFixed(2)} Mpix/s wall)`);
}
