import type { FractalSpec } from "@/lib/api/platform";
import { LOCAL_VARIANTS, type LocalMetric, type LocalRenderSpec, type LocalVariant } from "./local-render-core";
import { compileColorProgram } from "./local-field-cache";
import { compileLocalOrbitProgram } from "./local-orbit-program";
import { renderWebGpuRgba } from "./webgpu-renderer";

const variants = new Set<string>(LOCAL_VARIANTS);
const axisVariants = new Set<string>(LOCAL_VARIANTS.slice(0, 10));
const metrics = new Set<string>(["escape", "min_abs", "max_abs", "envelope", "min_pairwise_dist", "mandel_ship_agree"]);
const colorMaps = new Set(["classic_cos", "mod17", "hsv_wheel", "tri765", "grayscale", "hs_rainbow", "inferno", "viridis", "twilight", "ember_blue", "spectral1530"]);
const colorModes = new Set(["direct", "eq_full", "eq_center"]);
let sequence = 0;

export type LocalRenderChannel = "main" | "julia_picker";
export type LocalPreviewCallback = (blob: Blob, dimensions: { width: number; height: number }) => void;

function finite(value: number): boolean { return Number.isFinite(value); }

export function localRenderSpec(spec: FractalSpec): LocalRenderSpec | null {
  const variant = spec.variant ?? "mandelbrot";
  const metric = spec.metric ?? "escape";
  const scale = Number(spec.scale ?? 3);
  const iterations = Math.round(Number(spec.iterations ?? 512));
  const colorMap = spec.colorMap ?? "classic_cos";
  const colorMode = spec.colorMode ?? "direct";
  const centerRe = Number(spec.centerReStr ?? spec.centerRe ?? -0.75);
  const centerIm = Number(spec.centerImStr ?? spec.centerIm ?? 0);
  const rotationDeg = Number(spec.rotationDeg ?? 0);
  const juliaRe = Number(spec.juliaRe ?? 0);
  const juliaIm = Number(spec.juliaIm ?? 0);
  const cyclesPerOctave = Number(spec.cyclesPerOctave ?? 1);
  const scalarType = spec.scalarType ?? "auto";
  const engine = spec.engine ?? "auto";
  if (!variants.has(variant) || !metrics.has(metric) || !colorModes.has(colorMode)) return null;
  if (!spec.colorProgram && !colorMaps.has(colorMap)) return null;
  if (spec.colorProgram && colorMode !== "direct") return null;
  if (spec.orbitProgram && metric !== "escape") return null;
  // JavaScript numbers retain backend-compatible fp64 coordinates over the
  // Studio's normal navigation range. Deeper coordinates remain server-side.
  if (!finite(scale) || scale < 1e-12 || iterations < 1 || iterations > 5000) return null;
  if (![centerRe, centerIm, rotationDeg, juliaRe, juliaIm, cyclesPerOctave].every(finite) || cyclesPerOctave <= 0) return null;
  if (["fx64", "fp80", "fp128", "long_double"].includes(scalarType)) return null;
  if (["cuda", "hybrid"].includes(engine)) return null;

  const transcendental = ["sin_z", "cos_z", "exp_z", "sinh_z", "cosh_z", "tan_z"].includes(variant);
  const bailout = Number(spec.bailout ?? (transcendental ? 64 : 2));
  if (!finite(bailout) || bailout <= 0) return null;

  let orbitProgram: LocalRenderSpec["orbitProgram"];
  let colorProgram: LocalRenderSpec["colorProgram"];
  try {
    orbitProgram = spec.orbitProgram ? compileLocalOrbitProgram(spec.orbitProgram) : undefined;
    colorProgram = spec.colorProgram ? compileColorProgram(spec.colorProgram) : undefined;
  } catch { return null; }

  const transitionMode = spec.transitionMode ?? "off";
  let transition: LocalRenderSpec["transition"];
  if (transitionMode !== "off") {
    if (orbitProgram || metric === "mandel_ship_agree") return null;
    const from = spec.transitionFrom ?? "mandelbrot";
    const to = spec.transitionTo ?? "burning_ship";
    if (!axisVariants.has(from) || !axisVariants.has(to)) return null;
    if (transitionMode === "multi") {
      const inputLegs = spec.transitionLegs ?? [];
      if (inputLegs.length < 1 || inputLegs.length > 4) return null;
      const legs = inputLegs.map((leg) => ({
        variant: leg.variant as LocalVariant,
        weight: Number(leg.weight),
      }));
      if (legs.some((leg) => !axisVariants.has(leg.variant) || !finite(leg.weight))
        || !legs.some((leg) => leg.weight > 0)) return null;
      transition = { mode: "multi", thetaMilliDeg: 0, from: from as LocalVariant, to: to as LocalVariant, legs };
    } else if (transitionMode === "pair") {
      const thetaMilliDeg = Math.round(Number(spec.transitionThetaMilliDeg ?? 0));
      if (!Number.isSafeInteger(thetaMilliDeg)) return null;
      transition = { mode: "pair", thetaMilliDeg, from: from as LocalVariant, to: to as LocalVariant };
    } else return null;
  }

  const pairwiseCap = Math.round(Number(spec.pairwiseCap ?? 64));
  if (metric === "min_pairwise_dist" && (!Number.isSafeInteger(pairwiseCap) || pairwiseCap < 1 || pairwiseCap > 64)) return null;
  return {
    centerRe,
    centerIm,
    scale,
    iterations,
    variant: variant as LocalVariant,
    metric: metric as LocalMetric,
    colorMap,
    colorMode,
    cyclesPerOctave,
    smooth: Boolean(spec.smooth),
    rotationDeg,
    julia: Boolean(spec.julia),
    juliaRe,
    juliaIm,
    bailout,
    pairwiseCap: metric === "min_pairwise_dist" ? pairwiseCap : 64,
    orbitProgram,
    colorProgram,
    transition,
  };
}

export function canRenderFractalLocally(spec: FractalSpec): boolean {
  if (typeof Worker === "undefined" || typeof OffscreenCanvas === "undefined") return false;
  const local = localRenderSpec(spec);
  if (!local) return false;
  if (spec.scalarType === "fp32" || spec.scalarType === "float") {
    return typeof navigator !== "undefined" && "gpu" in navigator
      && !local.transition && !local.colorProgram && !local.orbitProgram
      && LOCAL_VARIANTS.indexOf(local.variant) < 10
      && ["escape", "min_abs", "max_abs", "envelope"].includes(local.metric)
      && local.colorMode === "direct";
  }
  return true;
}

type WorkerResponse = {
  id: number;
  stage?: "preview" | "final";
  blob?: Blob;
  width?: number;
  height?: number;
  y0?: number;
  rgba?: Uint8ClampedArray;
  error?: string;
};

type PendingRender = {
  id: number;
  resolve: (blob: Blob | null) => void;
  reject: (reason: unknown) => void;
  signal?: AbortSignal;
  abort: () => void;
  onPreview?: LocalPreviewCallback;
  onTile?: (y0: number, rgba: Uint8ClampedArray) => void;
};

type WorkerSlot = {
  channel: LocalRenderChannel;
  worker: Worker;
  pending: PendingRender | null;
};

const workerSlots = new Map<LocalRenderChannel, WorkerSlot[]>();

function abortError(): DOMException { return new DOMException("Render aborted", "AbortError"); }

function clearPending(slot: WorkerSlot): PendingRender | null {
  const pending = slot.pending;
  if (!pending) return null;
  pending.signal?.removeEventListener("abort", pending.abort);
  slot.pending = null;
  return pending;
}

function discardSlot(slot: WorkerSlot, reason?: unknown): void {
  slot.worker.terminate();
  const pool = workerSlots.get(slot.channel);
  if (pool) {
    const index = pool.indexOf(slot);
    if (index >= 0) pool.splice(index, 1);
    if (pool.length === 0) workerSlots.delete(slot.channel);
  }
  const pending = clearPending(slot);
  if (pending && reason) pending.reject(reason);
}

function createWorkerSlot(channel: LocalRenderChannel): WorkerSlot {
  const worker = new Worker(new URL("../../workers/fractal-render.worker.ts", import.meta.url), { type: "module" });
  const slot: WorkerSlot = { channel, worker, pending: null };
  worker.onmessage = (event: MessageEvent<WorkerResponse>) => {
    const pending = slot.pending;
    if (!pending || event.data.id !== pending.id) return;
    if (event.data.error) {
      clearPending(slot)?.reject(new Error(event.data.error));
      return;
    }
    if (event.data.y0 !== undefined && event.data.rgba) {
      try {
        pending.onTile?.(event.data.y0, event.data.rgba);
      } catch { /* Tile publishing failure invalidates the final render. */ }
      return;
    }
    if (!event.data.blob) return;
    if (event.data.stage === "preview") {
      try {
        pending.onPreview?.(event.data.blob, {
          width: event.data.width ?? 0,
          height: event.data.height ?? 0,
        });
      } catch { /* Preview publishing cannot invalidate the final render. */ }
      return;
    }
    clearPending(slot)?.resolve(event.data.blob);
  };
  worker.onerror = (event) => discardSlot(slot, new Error(event.message || "local_render_worker_failed"));
  return slot;
}

function dimensionsForRender(local: LocalRenderSpec, width: number, height: number) {
  const heavyEdge = local.metric === "min_pairwise_dist" || local.orbitProgram ? 384
    : local.transition || local.metric === "mandel_ship_agree" || LOCAL_VARIANTS.indexOf(local.variant) >= 10 ? 512
    : Number.POSITIVE_INFINITY;
  const renderScale = Math.min(1, heavyEdge / Math.max(width, height));
  const renderWidth = Math.max(1, Math.round(width * renderScale));
  const renderHeight = Math.max(1, Math.round(height * renderScale));
  const previewScale = Math.min(1, 256 / Math.max(renderWidth, renderHeight));
  return {
    width: renderWidth,
    height: renderHeight,
    previewWidth: Math.max(1, Math.round(renderWidth * previewScale)),
    previewHeight: Math.max(1, Math.round(renderHeight * previewScale)),
  };
}

async function rgbaToBlob(rgba: Uint8ClampedArray, width: number, height: number): Promise<Blob> {
  const canvas = new OffscreenCanvas(width, height);
  const context = canvas.getContext("2d", { alpha: false });
  if (!context) throw new Error("2d_context_unavailable");
  context.putImageData(new ImageData(rgba as Uint8ClampedArray<ArrayBuffer>, width, height), 0, 0);
  return canvas.convertToBlob({ type: "image/png" });
}

function tiledEligible(local: LocalRenderSpec): boolean {
  return local.colorMode === "direct"
    && local.metric !== "mandel_ship_agree"
    && local.metric !== "min_pairwise_dist"
    && !local.transition
    && !local.orbitProgram
    && !local.colorProgram
    && LOCAL_VARIANTS.indexOf(local.variant) < 10;
}

function workerCount(): number {
  if (typeof navigator === "undefined") return 4;
  const cores = navigator.hardwareConcurrency ?? 4;
  // Reserve one core for the main thread; cap the pool at 8 workers.
  return Math.max(1, Math.min(cores - 1, 8));
}

function ensurePool(channel: LocalRenderChannel): WorkerSlot[] {
  const existing = workerSlots.get(channel);
  if (existing) return existing;
  const pool: WorkerSlot[] = [];
  for (let index = 0; index < workerCount(); index += 1) {
    pool.push(createWorkerSlot(channel));
  }
  workerSlots.set(channel, pool);
  return pool;
}

function renderInWorker(
  local: LocalRenderSpec,
  dimensions: ReturnType<typeof dimensionsForRender>,
  signal: AbortSignal | undefined,
  channel: LocalRenderChannel,
  onPreview: LocalPreviewCallback | undefined,
): Promise<Blob | null> {
  const pool = ensurePool(channel);
  const slot = pool[0] ?? createWorkerSlot(channel);
  if (slot.pending) discardSlot(slot, abortError());
  const id = ++sequence;
  return new Promise((resolve, reject) => {
    const abort = () => {
      if (slot.pending?.id === id) discardSlot(slot, abortError());
    };
    if (signal?.aborted) { reject(abortError()); return; }
    slot.pending = { id, resolve, reject, signal, abort, onPreview };
    signal?.addEventListener("abort", abort, { once: true });
    slot.worker.postMessage({ id, spec: local, ...dimensions });
  });
}

function renderTiledInWorkers(
  local: LocalRenderSpec,
  dimensions: ReturnType<typeof dimensionsForRender>,
  signal: AbortSignal | undefined,
  channel: LocalRenderChannel,
  onPreview: LocalPreviewCallback | undefined,
): Promise<Blob | null> {
  const pool = ensurePool(channel);
  const { width, height } = dimensions;
  const workers = Math.max(1, Math.min(pool.length, height));
  const tileHeight = Math.ceil(height / workers);
  const id = ++sequence;
  const used = pool.slice(0, workers);
  return new Promise((resolve, reject) => {
    const tiles: Array<{ y0: number; rgba: Uint8ClampedArray }> = [];
    let completed = 0;
    let rejected = false;
    const compose = () => {
      if (rejected || completed < workers) return;
      // All tiles are in: release the pool slots so the next render can
      // reuse the workers instead of discarding them.
      for (const slot of used) clearPending(slot);
      try {
        const frame = new Uint8ClampedArray(width * height * 4);
        for (const tile of tiles) {
          frame.set(tile.rgba, tile.y0 * width * 4);
        }
        resolve(rgbaToBlob(frame, width, height));
      } catch (reason) {
        reject(reason);
      }
    };
    const abort = () => {
      for (const slot of used) {
        if (slot.pending?.id === id) discardSlot(slot, abortError());
      }
    };
    if (signal?.aborted) { reject(abortError()); return; }
    signal?.addEventListener("abort", abort, { once: true });
    for (let w = 0; w < workers; w += 1) {
      const slot = used[w] ?? createWorkerSlot(channel);
      if (slot.pending) discardSlot(slot, abortError());
      const y0 = w * tileHeight;
      const tileH = Math.min(tileHeight, height - y0);
      slot.pending = {
        id,
        resolve: () => { /* tiles are assembled through onTile */ },
        reject: (reason) => { rejected = true; reject(reason); },
        signal,
        abort,
        onPreview,
        onTile: (tileY0, rgba) => {
          tiles.push({ y0: tileY0, rgba });
          completed += 1;
          compose();
        },
      };
      slot.worker.postMessage({
        id,
        spec: local,
        width,
        height: tileH,
        y0,
        tileHeight: tileH,
        frameHeight: height,
      });
    }
  });
}

let wasmProbe: Promise<boolean> | null = null;

async function detectWasm(): Promise<boolean> {
  wasmProbe ??= (async () => {
    try {
      const url = new URL("../../scripts/wasm-core/field_core.wasm", import.meta.url);
      const bytes = await (await fetch(url)).arrayBuffer();
      await WebAssembly.instantiate(bytes, {});
      return true;
    } catch {
      return false;
    }
  })();
  return wasmProbe;
}

// Device-tier routing: weak devices (few cores and no wasm SIMD core) hand
// heavy renders to the compute nodes instead of stalling the browser with a
// single-threaded JS render. Strong devices keep everything local.
async function heavyRenderOffloaded(local: LocalRenderSpec, width: number, height: number): Promise<boolean> {
  if (typeof navigator === "undefined") return false;
  const cores = navigator.hardwareConcurrency ?? 1;
  if (cores >= 4) return false;
  const heavy = width * height > 1_000_000 || local.iterations > 2000;
  if (!heavy) return false;
  const hasWasm = await detectWasm();
  return !hasWasm;
}

export async function renderFractalLocally(
  spec: FractalSpec,
  width: number,
  height: number,
  signal?: AbortSignal,
  channel: LocalRenderChannel = "main",
  onPreview?: LocalPreviewCallback,
): Promise<Blob | null> {
  const local = localRenderSpec(spec);
  if (!local || typeof Worker === "undefined" || typeof OffscreenCanvas === "undefined") return null;
  if (!Number.isSafeInteger(width) || !Number.isSafeInteger(height) || width < 1 || height < 1
    || width > 4096 || height > 4096 || width * height > 4_194_304) return null;
  if (await heavyRenderOffloaded(local, width, height)) return null;
  const dimensions = dimensionsForRender(local, width, height);
  const requestedScalar = spec.scalarType ?? "auto";
  const allowWebGpu = requestedScalar === "auto" || requestedScalar === "fp32" || requestedScalar === "float";
  if (allowWebGpu && !local.transition && !local.colorProgram) {
    const gpuRgba = await renderWebGpuRgba(local, dimensions.width, dimensions.height, signal);
    if (gpuRgba) return rgbaToBlob(gpuRgba, dimensions.width, dimensions.height);
  }
  if (requestedScalar === "fp32" || requestedScalar === "float") return null;
  if (tiledEligible(local) && dimensions.height >= 4) {
    const previewOnly = dimensions.previewWidth && dimensions.previewHeight
      && (dimensions.previewWidth !== dimensions.width || dimensions.previewHeight !== dimensions.height);
    if (previewOnly) {
      // Progressive preview on the first pool worker, then tiled final.
      const pool = ensurePool(channel);
      const slot = pool[0] ?? createWorkerSlot(channel);
      const previewId = ++sequence;
      await new Promise<void>((resolvePreview, rejectPreview) => {
        if (signal?.aborted) { rejectPreview(abortError()); return; }
        const abort = () => { if (slot.pending?.id === previewId) discardSlot(slot, abortError()); };
        slot.pending = {
          id: previewId,
          resolve: () => {},
          reject: rejectPreview,
          signal,
          abort,
          // The worker's preview reply only publishes the blob, so the
          // completion hook rides on the preview callback: clear the slot so
          // the tiled final pass can reuse this worker.
          onPreview: (blob, blobDimensions) => {
            clearPending(slot);
            onPreview?.(blob, blobDimensions);
            resolvePreview();
          },
        };
        signal?.addEventListener("abort", abort, { once: true });
        slot.worker.postMessage({
          id: previewId,
          spec: local,
          width: dimensions.previewWidth,
          height: dimensions.previewHeight,
          previewOnly: true,
        });
      });
    }
    return renderTiledInWorkers(local, dimensions, signal, channel, onPreview);
  }
  return renderInWorker(local, dimensions, signal, channel, onPreview);
}
