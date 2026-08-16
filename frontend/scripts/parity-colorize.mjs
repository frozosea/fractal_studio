// wasm colorize vs TS colorizeLocalRawField: per-pixel RGBA parity across
// all palettes x smooth x escape/metric. Run: node scripts/parity-colorize.mjs

import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import ts from "typescript";

const ROOT = resolve(import.meta.dirname, "..");
function dataUrl(relative, replaces = []) {
  const source = readFileSync(resolve(ROOT, relative), "utf8");
  let compiled = ts.transpileModule(source, { compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2022 } }).outputText;
  for (const [from, to] of replaces) compiled = compiled.replace(from, to);
  return `data:text/javascript;base64,${Buffer.from(compiled).toString("base64")}`;
}
const orbitUrl = dataUrl("src/lib/fractal/local-orbit-program.ts");
const cacheUrl = dataUrl("src/lib/fractal/local-field-cache.ts");
const coreUrl = dataUrl("src/lib/fractal/local-render-core.ts", [
  ['"./local-orbit-program"', JSON.stringify(orbitUrl)],
  ['"./local-field-cache"', JSON.stringify(cacheUrl)],
]);
const core = await import(coreUrl);
const wasmUrl = dataUrl("src/lib/fractal/wasm-field-core.ts", [
  ['"./local-render-core"', JSON.stringify(coreUrl)],
  ['"./local-field-cache"', JSON.stringify(cacheUrl)],
]);
const wasm = await import(wasmUrl);

console.log("fieldBytes len:", readFileSync(resolve(ROOT, "scripts/wasm-core/field_core.wasm")).length);
console.log("colorBytes len:", readFileSync(resolve(ROOT, "scripts/wasm-core/colorize.wasm")).length);
let fieldInst, colorInst;
try {
  fieldInst = await WebAssembly.instantiate(readFileSync(resolve(ROOT, "scripts/wasm-core/field_core.wasm")), {});
  colorInst = await WebAssembly.instantiate(readFileSync(resolve(ROOT, "scripts/wasm-core/colorize.wasm")), {});
} catch (e) {
  console.error("wasm load error:", e);
  process.exit(1);
}
console.log("typeof colorInst:", typeof colorInst, "typeof fieldInst:", typeof fieldInst);
const colorizeFn = colorInst.instance.exports.field_core_colorize;
const mem = colorInst.instance.exports.memory;

const PALETTES = ["classic_cos", "mod17", "hsv_wheel", "tri765", "grayscale", "hs_rainbow", "inferno", "viridis", "twilight", "ember_blue", "spectral1530"];

function wasmColorize(raw, spec, width, height) {
  const count = width * height;
  // Keep the scratch area above the wasm data segment (gradient tables live
  // at the start of memory; writing there corrupts them).
  const base = 1024 * 1024;
  const log2log2Ptr = base;
  const log2fieldPtr = base + count * 8;
  const rgbaPtr = log2fieldPtr + count * 8;
  const itersPtr = rgbaPtr + count * 4;
  const normsPtr = itersPtr + count * 4;
  const fieldsPtr = normsPtr + count * 4;
  const log2log2 = new Float64Array(mem.buffer, log2log2Ptr, count);
  const log2field = new Float64Array(mem.buffer, log2fieldPtr, count);
  for (let i = 0; i < count; i += 1) {
    const norm = raw.kind === "escape" ? raw.norms[i] : 0;
    const field = raw.kind === "metric" ? raw.values[i] : 0;
    log2log2[i] = norm > 1 ? Math.log2(Math.log2(norm)) : 0;
    log2field[i] = field > 0 ? Math.log2(field) : 0;
  }
  if (raw.kind === "escape") {
    new Uint32Array(mem.buffer, itersPtr, count).set(raw.iterations);
    new Float32Array(mem.buffer, normsPtr, count).set(raw.norms);
    new Float64Array(mem.buffer, fieldsPtr, count).fill(0);
  } else {
    new Uint32Array(mem.buffer, itersPtr, count).fill(spec.iterations);
    new Float32Array(mem.buffer, normsPtr, count).fill(0);
    new Float64Array(mem.buffer, fieldsPtr, count).set(raw.values);
  }
  try {
    colorizeFn(itersPtr, normsPtr, fieldsPtr, log2log2Ptr, log2fieldPtr, count,
      spec.iterations, spec.metric === "escape" ? 0 : 1, spec.smooth ? 1 : 0,
      PALETTES.indexOf(spec.colorMap), spec.bailout, rgbaPtr);
  } catch (e) {
    console.error("colorize crash:", spec.colorMap, spec.metric, "smooth:", spec.smooth,
      "count:", count, "fieldsEnd:", fieldsPtr + count * 8, "memBytes:", mem.buffer.byteLength);
    throw e;
  }
  return new Uint8ClampedArray(mem.buffer, rgbaPtr, count * 4);
}

let failed = 0;
let total = 0;
let maxDelta = 0;
for (const palette of PALETTES) {
  for (const smooth of [false, true]) {
    for (const metric of ["escape", "min_abs", "max_abs", "envelope"]) {
      const spec = {
        centerRe: -0.743643887037151, centerIm: 0.13182590420533, scale: 1e-4,
        iterations: 400, variant: "mandelbrot", metric,
        colorMap: palette, colorMode: "direct", cyclesPerOctave: 1,
        smooth, rotationDeg: 0, julia: false, juliaRe: 0, juliaIm: 0,
        bailout: 2, pairwiseCap: 64,
      };
      const width = 128, height = 96;
      const raw = wasm.renderWasmRawField(spec, width, height, fieldInst.instance.exports);
      const jsRgba = core.colorizeLocalRawField(spec, raw);
      const wasmRgba = wasmColorize(raw, spec, width, height);
      let bad = 0;
      for (let i = 0; i < jsRgba.length; i += 1) {
        const d = Math.abs(jsRgba[i] - wasmRgba[i]);
        maxDelta = Math.max(maxDelta, d);
        if (d > 1) bad += 1;
      }
      total += 1;
      const fraction = bad / jsRgba.length;
      if (fraction > 0.001) {
        failed += 1;
        console.error(`FAIL ${palette}/${metric}/smooth=${smooth}: bad=${bad}/${jsRgba.length} (${(fraction * 100).toFixed(3)}%)`);
      }
    }
  }
}
if (failed) {
  console.error(`wasm/TS colorize: ${failed}/${total} cases failed, maxDelta=${maxDelta}`);
  process.exit(1);
}
console.log(`wasm/TS colorize: ${total} cases passed (11 palettes x 4 metrics x smooth, maxDelta=${maxDelta})`);
