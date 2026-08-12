import type { FractalSpec } from "@/lib/api/platform";
import { LOCAL_VARIANTS, type LocalMetric, type LocalRenderSpec, type LocalVariant } from "./local-render-core";

const variants = new Set<string>(LOCAL_VARIANTS);
const metrics = new Set<string>(["escape", "min_abs", "max_abs", "envelope"]);
const colorMaps = new Set(["classic_cos", "mod17", "hsv_wheel", "tri765", "grayscale", "hs_rainbow", "inferno", "viridis", "twilight", "ember_blue", "spectral1530"]);
let sequence = 0;

export function localRenderSpec(spec: FractalSpec): LocalRenderSpec | null {
  const variant = spec.variant ?? "mandelbrot";
  const metric = spec.metric ?? "escape";
  const scale = Number(spec.scale ?? 3);
  const iterations = Math.round(Number(spec.iterations ?? 512));
  const colorMap = spec.colorMap ?? "classic_cos";
  if (!variants.has(variant) || !metrics.has(metric) || !colorMaps.has(colorMap)) return null;
  if (spec.orbitProgram || spec.transitionMode && spec.transitionMode !== "off" || spec.colorProgram) return null;
  // JavaScript numbers retain backend-compatible fp64 coordinates over the
  // Studio's current navigation range. Very large iteration counts stay on
  // the cancellable native service instead of monopolising a user's device.
  if (!Number.isFinite(scale) || scale < 1e-12 || iterations < 1 || iterations > 5000) return null;
  const transcendental = ["sin_z", "cos_z", "exp_z", "sinh_z", "cosh_z", "tan_z"].includes(variant);
  return {
    centerRe: Number(spec.centerReStr ?? spec.centerRe ?? -0.75),
    centerIm: Number(spec.centerImStr ?? spec.centerIm ?? 0),
    scale,
    iterations,
    variant: variant as LocalVariant,
    metric: metric as LocalMetric,
    colorMap,
    colorMode: spec.colorMode ?? "direct",
    cyclesPerOctave: Number(spec.cyclesPerOctave ?? 1),
    smooth: Boolean(spec.smooth),
    rotationDeg: Number(spec.rotationDeg ?? 0),
    julia: Boolean(spec.julia),
    juliaRe: Number(spec.juliaRe ?? 0),
    juliaIm: Number(spec.juliaIm ?? 0),
    bailout: Number(spec.bailout ?? (transcendental ? 64 : 2)),
  };
}

export function canRenderFractalLocally(spec: FractalSpec): boolean {
  return typeof Worker !== "undefined" && typeof OffscreenCanvas !== "undefined" && localRenderSpec(spec) !== null;
}

export function renderFractalLocally(spec: FractalSpec, width: number, height: number, signal?: AbortSignal): Promise<Blob | null> {
  const local = localRenderSpec(spec);
  if (!local || typeof Worker === "undefined" || typeof OffscreenCanvas === "undefined") return Promise.resolve(null);
  const id = ++sequence;
  const worker = new Worker(new URL("../../workers/fractal-render.worker.ts", import.meta.url), { type: "module" });
  return new Promise((resolve, reject) => {
    const finish = () => worker.terminate();
    const abort = () => { finish(); reject(new DOMException("Render aborted", "AbortError")); };
    if (signal?.aborted) { abort(); return; }
    signal?.addEventListener("abort", abort, { once: true });
    worker.onmessage = (event: MessageEvent<{ id: number; blob?: Blob; error?: string }>) => {
      if (event.data.id !== id) return;
      signal?.removeEventListener("abort", abort); finish();
      if (event.data.error || !event.data.blob) reject(new Error(event.data.error ?? "local_render_failed"));
      else resolve(event.data.blob);
    };
    worker.onerror = (event) => { signal?.removeEventListener("abort", abort); finish(); reject(new Error(event.message)); };
    worker.postMessage({ id, spec: local, width, height });
  });
}
