// wasm field_core vs TS local-render-core: per-pixel iteration/norm/field
// parity across all 10 2D variants x 4 metrics x julia x rotation.
// Run: node scripts/parity-wasm-core.mjs

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

const wasmBytes = readFileSync(resolve(ROOT, "scripts/wasm-core/field_core.wasm"));
const { instance } = await WebAssembly.instantiate(wasmBytes, {});
const { field_core_render, memory } = instance.exports;

const VARIANTS = ["mandelbrot", "tricorn", "burning_ship", "celtic", "heart",
  "buffalo", "perp_buffalo", "celtic_ship", "mandelceltic", "perp_ship"];
const METRICS = ["escape", "min_abs", "max_abs", "envelope"];

function wasmField(spec, width, height) {
  const itersPtr = 0;
  const normsPtr = width * height * 4;
  const fieldsPtr = normsPtr + width * height * 4;
  const angle = spec.rotationDeg * Math.PI / 180;
  field_core_render(
    spec.centerRe, spec.centerIm, spec.scale, width / height,
    Math.cos(angle), Math.sin(angle),
    spec.iterations, spec.bailout,
    core.LOCAL_VARIANTS.indexOf(spec.variant), METRICS.indexOf(spec.metric),
    spec.julia ? 1 : 0, spec.juliaRe, spec.juliaIm,
    width, height, 0, height,
    itersPtr, normsPtr, fieldsPtr,
  );
  return {
    iterations: new Uint32Array(memory.buffer, itersPtr, width * height),
    norms: new Float32Array(memory.buffer, normsPtr, width * height),
    fields: new Float64Array(memory.buffer, fieldsPtr, width * height),
  };
}

let failed = 0;
let total = 0;
for (const variant of VARIANTS) {
  for (const metric of METRICS) {
    for (const julia of [false, true]) {
      for (const rotationDeg of [0, 37]) {
        const spec = {
          centerRe: julia ? -0.5 : -0.743643887037151,
          centerIm: julia ? 0.1 : 0.13182590420533,
          scale: julia ? 2.2 : 1e-4,
          iterations: 400,
          variant, metric,
          colorMap: "classic_cos", colorMode: "direct", cyclesPerOctave: 1,
          smooth: false, rotationDeg,
          julia, juliaRe: -0.8, juliaIm: 0.156,
          bailout: 2, pairwiseCap: 64,
        };
        const width = 128, height = 96;
        const wasm = wasmField(spec, width, height);
        const ts = core.computeLocalRawField(spec, width, height);
        let iterBad = 0, normBad = 0, fieldBad = 0;
        const isEscape = metric === "escape";
        for (let i = 0; i < width * height; i += 1) {
          // TS computeLocalRawField emits iterations/norms only for escape;
          // metric modes emit only the field values.
          if (isEscape) {
            if (wasm.iterations[i] !== ts.iterations[i]) iterBad += 1;
            if (Math.abs(wasm.norms[i] - ts.norms[i]) > 1e-5 * Math.max(1, Math.abs(ts.norms[i]))) normBad += 1;
          } else {
            if (Math.abs(wasm.fields[i] - ts.values[i]) > 1e-9 * Math.max(1, Math.abs(ts.values[i]))) fieldBad += 1;
          }
        }
        total += 1;
        const ok = iterBad === 0 && normBad === 0 && fieldBad === 0;
        if (!ok) {
          failed += 1;
          console.error(`FAIL ${variant}/${metric} julia=${julia} rot=${rotationDeg}: iter=${iterBad} norm=${normBad} field=${fieldBad}`);
        }
      }
    }
  }
}
if (failed) {
  console.error(`wasm/TS parity: ${failed}/${total} cases failed`);
  process.exit(1);
}
console.log(`wasm/TS parity: ${total} cases passed (10 variants x 4 metrics x julia x rotation, per-pixel exact)`);
