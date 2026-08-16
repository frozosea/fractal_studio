/// <reference lib="webworker" />

import {
  colorizeLocalAgreement,
  colorizeLocalRawField,
  computeLocalRawField,
  type LocalRenderSpec,
} from "@/lib/fractal/local-render-core";
import {
  isWasmEligible,
  loadWasmColorizeCore,
  loadWasmFieldCore,
  renderWasmColorize,
  renderWasmRawField,
} from "@/lib/fractal/wasm-field-core";
import {
  createRawFieldCacheKey,
  RawFieldCache,
  type RawEscapeField,
  type RawField,
  type RawMetricField,
} from "@/lib/fractal/local-field-cache";
import {
  renderLocalMandelShipAgreementRaw,
  renderLocalTransitionRaw,
  type LocalAxisTransitionVariant,
  type LocalTransitionMetric,
  type LocalTransitionRenderSpec,
} from "@/lib/fractal/local-transition-core";

type RenderMessage = {
  id: number;
  spec: LocalRenderSpec;
  width: number;
  height: number;
  previewWidth?: number;
  previewHeight?: number;
  // Tiled final render: render tileHeight rows starting at global row y0 and
  // post back the raw RGBA (transferable) instead of a PNG blob.
  y0?: number;
  tileHeight?: number;
  frameHeight?: number;
  previewOnly?: boolean;
};

type RenderStage = "preview" | "final";
const fieldCache = new RawFieldCache(64 * 1024 * 1024);

async function rgbaBlob(rgba: Uint8ClampedArray, width: number, height: number): Promise<Blob> {
  const canvas = new OffscreenCanvas(width, height);
  const context = canvas.getContext("2d", { alpha: false });
  if (!context) throw new Error("2d_context_unavailable");
  context.putImageData(new ImageData(rgba as Uint8ClampedArray<ArrayBuffer>, width, height), 0, 0);
  return canvas.convertToBlob({ type: "image/png" });
}

function transitionSpec(spec: LocalRenderSpec): LocalTransitionRenderSpec {
  const transition = spec.transition;
  if (!transition) throw new Error("missing_transition_spec");
  return {
    centerRe: spec.centerRe,
    centerIm: spec.centerIm,
    scale: spec.scale,
    iterations: spec.iterations,
    metric: spec.metric as LocalTransitionMetric,
    bailout: spec.bailout,
    rotationDeg: spec.rotationDeg,
    julia: spec.julia,
    juliaRe: spec.juliaRe,
    juliaIm: spec.juliaIm,
    transitionThetaMilliDeg: transition.thetaMilliDeg,
    transitionFrom: transition.from as LocalAxisTransitionVariant,
    transitionTo: transition.to as LocalAxisTransitionVariant,
    transitionLegs: transition.mode === "multi" ? transition.legs?.map((leg) => ({
      variant: leg.variant as LocalAxisTransitionVariant,
      weight: leg.weight,
    })) : undefined,
    pairwiseCap: spec.pairwiseCap,
  };
}

let wasmFieldCore: Awaited<ReturnType<typeof loadWasmFieldCore>> | null = null;
let wasmColorizeCore: Awaited<ReturnType<typeof loadWasmColorizeCore>> | null = null;

async function colorizeRawAsync(spec: LocalRenderSpec, raw: RawField): Promise<Uint8ClampedArray> {
  if (isWasmEligible(spec) && spec.colorMode === "direct") {
    wasmColorizeCore ??= await loadWasmColorizeCore();
    if (wasmColorizeCore) {
      try {
        return renderWasmColorize(raw, spec, wasmColorizeCore);
      } catch { /* fall through to the TypeScript colorizer */ }
    }
  }
  return colorizeLocalRawField(spec, raw);
}

async function computeRawAsync(
  spec: LocalRenderSpec,
  width: number,
  height: number,
  y0 = 0,
  tileHeight = height,
): Promise<RawField> {
  if (isWasmEligible(spec)) {
    wasmFieldCore ??= await loadWasmFieldCore();
    if (wasmFieldCore) {
      try {
        return renderWasmRawField(spec, width, height, wasmFieldCore, y0, tileHeight);
      } catch { /* fall through to the TypeScript core */ }
    }
  }
  if (y0 !== 0 || tileHeight !== height) throw new Error("tiled_render_requires_wasm");
  return computeRaw(spec, width, height);
}

function computeRaw(spec: LocalRenderSpec, width: number, height: number): RawField {
  if (spec.transition) {
    const raw = renderLocalTransitionRaw(transitionSpec(spec), width, height);
    if (raw.iterU32 && raw.normF32) return {
      kind: "escape", metric: "escape", width, height, bailout: spec.bailout,
      iterationLimit: spec.iterations, iterations: raw.iterU32, norms: raw.normF32,
    } satisfies RawEscapeField;
    if (!raw.fieldF64) throw new Error("transition_field_missing");
    return {
      kind: "metric", metric: spec.metric, width, height, bailout: spec.bailout,
      values: raw.fieldF64,
    } satisfies RawMetricField;
  }
  return computeLocalRawField(spec, width, height);
}

function renderAgreement(spec: LocalRenderSpec, width: number, height: number): Uint8ClampedArray {
  const raw = renderLocalMandelShipAgreementRaw({
    centerRe: spec.centerRe,
    centerIm: spec.centerIm,
    scale: spec.scale,
    iterations: spec.iterations,
    variant: spec.variant,
    bailout: spec.bailout,
    julia: spec.julia,
    juliaRe: spec.juliaRe,
    juliaIm: spec.juliaIm,
    rotationDeg: spec.rotationDeg,
  }, width, height);
  if (!raw.agreementIterU32 || !raw.fieldF64) throw new Error("agreement_field_missing");
  return colorizeLocalAgreement(spec, raw.agreementIterU32, raw.fieldF64, width, height);
}

async function renderStage(
  id: number,
  stage: RenderStage,
  spec: LocalRenderSpec,
  width: number,
  height: number,
): Promise<void> {
  let rgba: Uint8ClampedArray;
  let cacheHit = false;
  if (spec.metric === "mandel_ship_agree") {
    rgba = renderAgreement(spec, width, height);
  } else {
    const cacheKey = createRawFieldCacheKey(spec as unknown as Record<string, unknown>, width, height);
    let raw = fieldCache.get(cacheKey);
    cacheHit = raw !== undefined;
    if (!raw) {
      raw = await computeRawAsync(spec, width, height);
      fieldCache.set(cacheKey, raw);
    }
    rgba = await colorizeRawAsync(spec, raw);
  }
  const blob = await rgbaBlob(rgba, width, height);
  self.postMessage({ id, stage, blob, width, height, cacheHit });
}

async function renderTileStage(
  id: number,
  spec: LocalRenderSpec,
  width: number,
  tileHeight: number,
  y0: number,
  frameHeight: number,
): Promise<void> {
  const cacheKey = `${createRawFieldCacheKey(spec as unknown as Record<string, unknown>, width, tileHeight)}:y${y0}`;
  let raw = fieldCache.get(cacheKey);
  if (!raw) {
    raw = await computeRawAsync(spec, width, frameHeight, y0, tileHeight);
    fieldCache.set(cacheKey, raw);
  }
  const rgba = await colorizeRawAsync(spec, raw);
  // Transfer the tile RGBA to the main thread for compositing. Copy first:
  // wasm-backed views share the WebAssembly.Memory buffer, which cannot be
  // detached.
  const transferable = new Uint8ClampedArray(rgba);
  self.postMessage({ id, y0, rgba: transferable, width, height: tileHeight }, { transfer: [transferable.buffer] });
}

self.onmessage = async (event: MessageEvent<RenderMessage>) => {
  const { id, spec, width, height, previewWidth, previewHeight, y0, tileHeight, frameHeight, previewOnly } = event.data;
  try {
    if (y0 !== undefined && tileHeight !== undefined && frameHeight !== undefined) {
      await renderTileStage(id, spec, width, tileHeight, y0, frameHeight);
      return;
    }
    if (previewOnly) {
      await renderStage(id, "preview", spec, width, height);
      return;
    }
    if (previewWidth && previewHeight && (previewWidth !== width || previewHeight !== height)) {
      await renderStage(id, "preview", spec, previewWidth, previewHeight);
    }
    await renderStage(id, "final", spec, width, height);
  } catch (reason) {
    self.postMessage({ id, error: reason instanceof Error ? reason.message : "local_render_failed" });
  }
};

export {};
