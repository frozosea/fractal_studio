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
  y0 = 0,
  tileHeight = height,
): RawEscapeField | RawMetricField {
  const count = width * tileHeight;
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
    width, height, y0, tileHeight,
    itersPtr, normsPtr, fieldsPtr,
  );
  if (spec.metric === "escape") {
    return {
      kind: "escape", metric: "escape", width, height: tileHeight, bailout: spec.bailout,
      iterationLimit: spec.iterations,
      iterations: new Uint32Array(memory.buffer, itersPtr, count),
      norms: new Float32Array(memory.buffer, normsPtr, count),
    } satisfies RawEscapeField;
  }
  return {
    kind: "metric", metric: spec.metric, width, height: tileHeight, bailout: spec.bailout,
    values: new Float64Array(memory.buffer, fieldsPtr, count),
  } satisfies RawMetricField;
}

export function loadWasmFieldCore(): Promise<WasmExports | null> {
  return ensureWasm();
}

// ---------------------------------------------------------------------------
// wasm colorize core (scripts/wasm-core/colorize.cpp): ports the TypeScript
// direct-mode coloring (escapeColor/metricColor/fieldColor) for all 11
// palettes x smooth x escape/metric. Parity-tested by
// scripts/parity-colorize.mjs (88 cases, maxDelta=1).
// ---------------------------------------------------------------------------

const PALETTES = ["classic_cos", "mod17", "hsv_wheel", "tri765", "grayscale",
  "hs_rainbow", "inferno", "viridis", "twilight", "ember_blue", "spectral1530"];

type ColorizeExports = {
  memory: WebAssembly.Memory;
  field_core_colorize: (
    iters: number, norms: number, fields: number,
    log2log2: number, log2field: number, count: number,
    iterations: number, metric: number, smooth: number,
    colorMap: number, bailout: number, outRgba: number,
  ) => void;
};

let colorizePromise: Promise<ColorizeExports | null> | null = null;

async function loadWasmColorize(): Promise<ColorizeExports | null> {
  try {
    const url = new URL("../../../scripts/wasm-core/colorize.wasm", import.meta.url);
    const bytes = await (await fetch(url)).arrayBuffer();
    const { instance } = await WebAssembly.instantiate(bytes, {});
    return instance.exports as unknown as ColorizeExports;
  } catch {
    return null;
  }
}

export function loadWasmColorizeCore(): Promise<ColorizeExports | null> {
  colorizePromise ??= loadWasmColorize();
  return colorizePromise;
}

export function renderWasmColorize(
  raw: RawEscapeField | RawMetricField,
  spec: LocalRenderSpec,
  exports: ColorizeExports,
): Uint8ClampedArray {
  const count = raw.width * raw.height;
  const memory = exports.memory;
  // Scratch area sits above the wasm data segment (gradient tables live at
  // the start of memory).
  const base = 1024 * 1024;
  const log2log2Ptr = base;
  const log2fieldPtr = base + count * 8;
  const rgbaPtr = log2fieldPtr + count * 8;
  const itersPtr = rgbaPtr + count * 4;
  const normsPtr = itersPtr + count * 4;
  const fieldsPtr = normsPtr + count * 4;
  const log2log2 = new Float64Array(memory.buffer, log2log2Ptr, count);
  const log2field = new Float64Array(memory.buffer, log2fieldPtr, count);
  for (let i = 0; i < count; i += 1) {
    const norm = raw.kind === "escape" ? (raw.norms[i] ?? 0) : 0;
    const field = raw.kind === "metric" ? (raw.values[i] ?? 0) : 0;
    log2log2[i] = norm > 1 ? Math.log2(Math.log2(norm)) : 0;
    log2field[i] = field > 0 ? Math.log2(field) : 0;
  }
  if (raw.kind === "escape") {
    new Uint32Array(memory.buffer, itersPtr, count).set(raw.iterations);
    new Float32Array(memory.buffer, normsPtr, count).set(raw.norms);
    new Float64Array(memory.buffer, fieldsPtr, count).fill(0);
  } else {
    new Uint32Array(memory.buffer, itersPtr, count).fill(spec.iterations);
    new Float32Array(memory.buffer, normsPtr, count).fill(0);
    new Float64Array(memory.buffer, fieldsPtr, count).set(raw.values);
  }
  exports.field_core_colorize(
    itersPtr, normsPtr, fieldsPtr, log2log2Ptr, log2fieldPtr, count,
    spec.iterations, spec.metric === "escape" ? 0 : 1, spec.smooth ? 1 : 0,
    PALETTES.indexOf(spec.colorMap), spec.bailout, rgbaPtr,
  );
  return new Uint8ClampedArray(memory.buffer, rgbaPtr, count * 4);
}
