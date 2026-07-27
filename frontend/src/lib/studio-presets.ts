import type { FractalSpec } from "@/lib/api/platform";

export type StudioPreset = { id: string; name: string; spec: Partial<FractalSpec> };

export const STUDIO_PRESETS: StudioPreset[] = [
  { id: "classic", name: "Classic coast", spec: { centerRe: -0.75, centerIm: 0, scale: 3, iterations: 256, variant: "mandelbrot", colorMap: "classic_cos" } },
  { id: "seahorse", name: "Seahorse valley", spec: { centerRe: -0.743643887, centerIm: 0.131825904, scale: 0.003, iterations: 900, variant: "mandelbrot", colorMap: "viridis", smooth: true } },
  { id: "spiral", name: "Spiral", spec: { centerRe: -0.761574, centerIm: -0.0847596, scale: 0.0017, iterations: 1200, variant: "mandelbrot", colorMap: "inferno", smooth: true } },
  { id: "elephant", name: "Elephant", spec: { centerRe: 0.285, centerIm: 0.01, scale: 0.08, iterations: 800, variant: "mandelbrot", colorMap: "twilight", smooth: true } },
  { id: "tricorn", name: "Tricorn bloom", spec: { centerRe: -0.4, centerIm: 0, scale: 3.2, iterations: 420, variant: "tricorn", colorMap: "spectral1530" } },
  { id: "ship", name: "Burning ship", spec: { centerRe: -1.75, centerIm: -0.03, scale: 2.1, iterations: 640, variant: "burning_ship", colorMap: "ember_blue", smooth: true } },
  { id: "celtic", name: "Celtic knots", spec: { centerRe: -0.1, centerIm: 0.65, scale: 2.8, iterations: 512, variant: "celtic", colorMap: "hsv_wheel" } },
  { id: "heart", name: "Heart", spec: { centerRe: -0.2, centerIm: 0, scale: 3.2, iterations: 384, variant: "heart", colorMap: "hs_rainbow" } },
  { id: "buffalo", name: "Buffalo", spec: { centerRe: -0.5, centerIm: 0, scale: 3.5, iterations: 512, variant: "buffalo", colorMap: "mod17" } },
  { id: "perp-buffalo", name: "Perpendicular buffalo", spec: { centerRe: -0.45, centerIm: 0.1, scale: 3.4, iterations: 512, variant: "perp_buffalo", colorMap: "tri765" } },
  { id: "celtic-ship", name: "Celtic ship", spec: { centerRe: -1.1, centerIm: -0.1, scale: 2.7, iterations: 620, variant: "celtic_ship", colorMap: "inferno" } },
  { id: "mandelceltic", name: "Mandelceltic", spec: { centerRe: -0.2, centerIm: 0.1, scale: 2.8, iterations: 512, variant: "mandelceltic", colorMap: "viridis" } },
  { id: "perp-ship", name: "Perpendicular ship", spec: { centerRe: -1.1, centerIm: 0.1, scale: 2.8, iterations: 620, variant: "perp_ship", colorMap: "ember_blue" } },
  { id: "sine", name: "Sine field", spec: { centerRe: 0, centerIm: 0, scale: 5, iterations: 300, variant: "sin_z", colorMap: "twilight" } },
  { id: "cosine", name: "Cosine field", spec: { centerRe: 0, centerIm: 0, scale: 5, iterations: 300, variant: "cos_z", colorMap: "spectral1530" } },
  { id: "exp", name: "Exponential", spec: { centerRe: -0.25, centerIm: 0, scale: 4, iterations: 320, variant: "exp_z", colorMap: "hsv_wheel" } },
  { id: "sinh", name: "Hyperbolic sine", spec: { centerRe: 0, centerIm: 0, scale: 5, iterations: 300, variant: "sinh_z", colorMap: "viridis" } },
  { id: "tangent", name: "Tangent", spec: { centerRe: 0, centerIm: 0, scale: 4.5, iterations: 320, variant: "tan_z", colorMap: "classic_cos" } },
];

export function randomPreset(): StudioPreset {
  return STUDIO_PRESETS[Math.floor(Math.random() * STUDIO_PRESETS.length)]!;
}

export function jitterPreset(spec: Partial<FractalSpec>, intensity: number): Partial<FractalSpec> {
  const factor = Math.max(1, Math.min(5, intensity));
  const scale = Number(spec.scale ?? 3);
  const jitter = scale * (0.015 * factor) * (Math.random() - 0.5);
  return {
    ...spec,
    centerRe: Number(spec.centerRe ?? 0) + jitter,
    centerIm: Number(spec.centerIm ?? 0) + jitter,
    scale: scale * (0.72 + Math.random() * 0.56),
    iterations: Math.min(4000, Math.round(Number(spec.iterations ?? 256) * (0.8 + factor * 0.18))),
    rotationDeg: Math.round((Math.random() - 0.5) * factor * 12),
  };
}
