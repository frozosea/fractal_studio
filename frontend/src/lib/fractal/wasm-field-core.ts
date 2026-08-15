// wasm SIMD field core integration.
//
// Renders the raw iteration/metric field with the C++ wasm core
// (scripts/wasm-core/field_core.cpp, built with clang -msimd128) which is
// parity-tested pixel-exact against local-render-core (see
// scripts/parity-wasm-core.mjs). The wasm path covers the 10 quadratic 2D
// variants with escape/min_abs/max_abs/envelope metrics; everything else
// (transcendental variants, orbit programs, transitions, agreement,
// min_pairwise_dist) falls back to the TypeScript core.
//
// The wasm output layout mirrors computeLocalRawField, so the existing
// colorizeLocalRawField consumes it unchanged.

import type { LocalMetric, LocalRenderSpec, LocalVariant } from "./local-render-core";
import type { RawEscapeField, RawMetricField } from "./local-field-cache";

const WASM_VARIANTS = new Set<LocalVariant>([
  "mandelbrot", "tricorn", "burning_ship", "celtic", "heart",
  "buffalo", "perp_buffalo", "celtic_ship", "mandelceltic", "perp_ship",
]);
const WASM_METRICS = new Set<LocalMetric>(["escape", "min_abs", "max_abs", "envelope"]);
const METRIC_INDEX: Record<string, number> = { escape: 0, min_abs: 1, max_abs: 2, envelope: 3 };
const VARIANT_INDEX = new Map<LocalVariant, number>(
  [...WASM_VARIANTS].map((name, index) => [name, index]),
);

// 1024x768 x (u32 + f32 + f64) = 12 MiB, within the wasm initial memory.
const MAX_PIXELS = 1024 * 768;

type WasmExports = {
  memory: WebAssembly.Memory;
  field_core_render: (
    centerRe: number, centerIm: number, scale: number, aspect: number,
    cosAngle: number, sinAngle: number,
    iterations: number, bailout: number,
    variant: number, metric: number,
    julia: number, juliaRe: number, juliaIm: number,
    width: number, globalHeight: number, y0: number, tileHeight: number,
    outIters: number, outNorms: number, outFields: number,
  ) => void;
};

export function isWasmEligible(spec: LocalRenderSpec): boolean {
  if (!WASM_VARIANTS.has(spec.variant) || !WASM_METRICS.has(spec.metric)) return false;
  if (spec.orbitProgram || spec.transition || spec.colorProgram) return false;
  return true;
}

let wasmPromise: Promise<WasmExports | null> | null = null;

async function loadWasm(): Promise<WasmExports | null> {
  try {
    const url = new URL("../../../scripts/wasm-core/field_core.wasm", import.meta.url);
    const bytes = await (await fetch(url)).arrayBuffer();
    const { instance } = await WebAssembly.instantiate(bytes, {});
    return instance.exports as unknown as WasmExports;
  } catch {
    return null;
  }
}

function ensureWasm(): Promise<WasmExports | null> {
  wasmPromise ??= loadWasm();
  return wasmPromise;
}

export function renderWasmRawField(
  spec: LocalRenderSpec,
  width: number,
  height: number,
  exports: WasmExports,
): RawEscapeField | RawMetricField {
  const count = width * height;
  if (count > MAX_PIXELS) throw new Error("wasm_field_too_large");
  const memory = exports.memory;
  const itersPtr = 0;
  const normsPtr = count * 4;
  const fieldsPtr = count * 8;
  const angle = spec.rotationDeg * Math.PI / 180;
  exports.field_core_render(
    spec.centerRe, spec.centerIm, spec.scale, width / height,
    Math.cos(angle), Math.sin(angle),
    spec.iterations, spec.bailout,
    VARIANT_INDEX.get(spec.variant) ?? 0, METRIC_INDEX[spec.metric] ?? 0,
    spec.julia ? 1 : 0, spec.juliaRe, spec.juliaIm,
    width, height, 0, height,
    itersPtr, normsPtr, fieldsPtr,
  );
  if (spec.metric === "escape") {
    return {
      kind: "escape", metric: "escape", width, height, bailout: spec.bailout,
      iterationLimit: spec.iterations,
      iterations: new Uint32Array(memory.buffer, itersPtr, count),
      norms: new Float32Array(memory.buffer, normsPtr, count),
    } satisfies RawEscapeField;
  }
  return {
    kind: "metric", metric: spec.metric, width, height, bailout: spec.bailout,
    values: new Float64Array(memory.buffer, fieldsPtr, count),
  } satisfies RawMetricField;
}

export function loadWasmFieldCore(): Promise<WasmExports | null> {
  return ensureWasm();
}
