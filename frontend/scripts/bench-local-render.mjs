// Local (browser-side) render benchmark: CPU fp64 worker core.
// Times renderLocalRgba across preview-sized workloads and reports Mpix/s
// so it can be compared with the node benchmark (docs/node_benchmark.md).
// Run: node scripts/bench-local-render.mjs

import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import ts from "typescript";

const ROOT = resolve(import.meta.dirname, "..");

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
  { name: "preview_512x384x512", width: 512, height: 384, iterations: 512 },
  { name: "preview_512x384x1500", width: 512, height: 384, iterations: 1500 },
  { name: "full_1024x768x512", width: 1024, height: 768, iterations: 512 },
  { name: "full_1024x768x1500", width: 1024, height: 768, iterations: 1500 },
];

const base = {
  centerRe: -0.743643887037151, centerIm: 0.13182590420533, scale: 1e-4,
  variant: "mandelbrot", metric: "escape", colorMap: "classic_cos",
  colorMode: "direct", cyclesPerOctave: 1, smooth: false, rotationDeg: 0,
  julia: false, juliaRe: 0, juliaIm: 0, bailout: 2, pairwiseCap: 64,
};

console.log("local CPU fp64 renderer (single worker thread):");
console.log(`${"case".padEnd(22)}${"ms".padStart(9)}${"Mpix/s".padStart(10)}${"Giter/s".padStart(10)}`);
for (const entry of CASES) {
  const spec = { ...base, iterations: entry.iterations };
  // warmup
  core.renderLocalRgba(spec, 64, 48);
  const runs = [];
  for (let i = 0; i < 3; i += 1) {
    const start = performance.now();
    core.renderLocalRgba(spec, entry.width, entry.height);
    runs.push(performance.now() - start);
  }
  const ms = Math.min(...runs);
  const pixels = entry.width * entry.height;
  const mpix = pixels / ms / 1000;
  const giter = (pixels * entry.iterations) / ms / 1e6;
  console.log(`${entry.name.padEnd(22)}${ms.toFixed(1).padStart(9)}${mpix.toFixed(2).padStart(10)}${giter.toFixed(2).padStart(10)}`);
}
