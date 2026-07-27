import type { FractalSpec } from "@/lib/api/platform";

export const BUILTIN_VARIANTS = [
  "mandelbrot",
  "tricorn",
  "burning_ship",
  "celtic",
  "heart",
  "buffalo",
  "perp_buffalo",
  "celtic_ship",
  "mandelceltic",
  "perp_ship",
  "sin_z",
  "cos_z",
  "exp_z",
  "sinh_z",
  "cosh_z",
  "tan_z",
] as const;

export const AXIS_TRANSITION_VARIANTS = BUILTIN_VARIANTS.slice(0, 10);

export const METRICS = [
  "escape",
  "min_abs",
  "max_abs",
  "envelope",
  "min_pairwise_dist",
  "mandel_ship_agree",
] as const;

export const TRANSITION_METRICS = ["escape", "min_abs", "max_abs", "envelope"] as const;

export const COLOR_MAPS = [
  { id: "classic_cos", preview: "linear-gradient(90deg,#071426,#2366aa,#f0a030,#f7e8b1)" },
  { id: "mod17", preview: "linear-gradient(90deg,#050505 0 24%,#bc3b2f 24% 49%,#e7a934 49% 74%,#e7e1ca 74%)" },
  { id: "hsv_wheel", preview: "linear-gradient(90deg,#e33,#ed3,#3c6,#3bd,#55e,#c4c,#e33)" },
  { id: "tri765", preview: "linear-gradient(90deg,#071426,#296ca3,#e2cf67,#b94a36,#071426)" },
  { id: "grayscale", preview: "linear-gradient(90deg,#050505,#f2f2f2)" },
  { id: "hs_rainbow", preview: "linear-gradient(90deg,#191036,#1f78b4,#42c6a5,#e7d64b,#df5d3f)" },
  { id: "inferno", preview: "linear-gradient(90deg,#000004,#420a68,#932667,#dd513a,#fca50a,#fcffa4)" },
  { id: "viridis", preview: "linear-gradient(90deg,#440154,#3b528b,#21918c,#5ec962,#fde725)" },
  { id: "twilight", preview: "linear-gradient(90deg,#e2d9e2,#5b4d9a,#161b3d,#7f3155,#e2d9e2)" },
  { id: "ember_blue", preview: "linear-gradient(90deg,#050820,#174b73,#37b4c3,#f0a030,#fff6d2)" },
  { id: "spectral1530", preview: "linear-gradient(90deg,#071426,#214d86,#279c8d,#e2cc56,#d55c3d,#8d2457)" },
] as const;

export const LOCATION_PRESETS: Array<{
  id: string;
  spec: Partial<FractalSpec>;
}> = [
  { id: "overview", spec: { centerRe: -0.75, centerIm: 0, scale: 3, iterations: 256 } },
  { id: "seahorse", spec: { centerRe: -0.743643887037151, centerIm: 0.13182590420533, scale: 0.003, iterations: 900 } },
  { id: "spiral", spec: { centerRe: -0.761574, centerIm: -0.0847596, scale: 0.0017, iterations: 1200 } },
  { id: "elephant", spec: { centerRe: 0.285, centerIm: 0.01, scale: 0.08, iterations: 800 } },
  { id: "needle", spec: { centerRe: -1.25066, centerIm: 0.02012, scale: 0.012, iterations: 1100 } },
  { id: "doubleSpiral", spec: { centerRe: -0.16, centerIm: 1.0405, scale: 0.026, iterations: 900 } },
];

export const OUTPUT_PRESETS = [
  { id: "square", width: 1024, height: 1024 },
  { id: "squareLarge", width: 2048, height: 2048 },
  { id: "landscape", width: 1600, height: 1200 },
  { id: "wide", width: 1920, height: 1080 },
  { id: "portrait", width: 1080, height: 1440 },
  { id: "fourK", width: 3840, height: 2160 },
] as const;

export function previewDimensions(width: number, height: number): { width: number; height: number } {
  const factor = Math.min(1, 768 / Math.max(width, height));
  return {
    width: Math.max(64, Math.round(width * factor)),
    height: Math.max(64, Math.round(height * factor)),
  };
}
