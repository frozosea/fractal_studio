export const LOCAL_VARIANTS = [
  "mandelbrot", "tricorn", "burning_ship", "celtic", "heart", "buffalo",
  "perp_buffalo", "celtic_ship", "mandelceltic", "perp_ship", "sin_z",
  "cos_z", "exp_z", "sinh_z", "cosh_z", "tan_z",
] as const;

export type LocalVariant = (typeof LOCAL_VARIANTS)[number];
export type LocalMetric = "escape" | "min_abs" | "max_abs" | "envelope";

export type LocalRenderSpec = {
  centerRe: number;
  centerIm: number;
  scale: number;
  iterations: number;
  variant: LocalVariant;
  metric: LocalMetric;
  colorMap: string;
  colorMode: "direct" | "eq_full" | "eq_center";
  cyclesPerOctave: number;
  smooth: boolean;
  rotationDeg: number;
  julia: boolean;
  juliaRe: number;
  juliaIm: number;
  bailout: number;
};

export type OrbitSample = { iter: number; norm: number; field: number };

const TRANSCENDENTAL = new Set<LocalVariant>(["sin_z", "cos_z", "exp_z", "sinh_z", "cosh_z", "tan_z"]);

function step(variant: LocalVariant, x: number, y: number, cx: number, cy: number): [number, number] {
  const x2 = x * x;
  const y2 = y * y;
  const xy2 = 2 * x * y;
  switch (variant) {
    case "mandelbrot": return [x2 - y2 + cx, xy2 + cy];
    case "tricorn": return [x2 - y2 + cx, -xy2 + cy];
    case "burning_ship": return [x2 - y2 + cx, 2 * Math.abs(x * y) + cy];
    case "celtic": return [x2 - y2 + cx, 2 * x * Math.abs(y) + cy];
    case "heart": return [x2 - y2 + cx, -2 * Math.abs(x) * y + cy];
    case "buffalo": return [Math.abs(x2 - y2) + cx, xy2 + cy];
    case "perp_buffalo": return [Math.abs(x2 - y2) + cx, -xy2 + cy];
    case "celtic_ship": return [Math.abs(x2 - y2) + cx, Math.abs(xy2) + cy];
    case "mandelceltic": return [Math.abs(x2 - y2) + cx, 2 * x * Math.abs(y) + cy];
    case "perp_ship": return [Math.abs(x2 - y2) + cx, -2 * Math.abs(x) * y + cy];
    case "sin_z": return [Math.sin(x) * Math.cosh(y) + cx, Math.cos(x) * Math.sinh(y) + cy];
    case "cos_z": return [Math.cos(x) * Math.cosh(y) + cx, -Math.sin(x) * Math.sinh(y) + cy];
    case "exp_z": { const e = Math.exp(x); return [e * Math.cos(y) + cx, e * Math.sin(y) + cy]; }
    case "sinh_z": return [Math.sinh(x) * Math.cos(y) + cx, Math.cosh(x) * Math.sin(y) + cy];
    case "cosh_z": return [Math.cosh(x) * Math.cos(y) + cx, Math.sinh(x) * Math.sin(y) + cy];
    case "tan_z": {
      const denominator = Math.cos(2 * x) + Math.cosh(2 * y);
      return denominator === 0 ? [cx, cy] : [Math.sin(2 * x) / denominator + cx, Math.sinh(2 * y) / denominator + cy];
    }
  }
}

export function iterateOrbit(spec: LocalRenderSpec, re: number, im: number): OrbitSample {
  let x = spec.julia ? re : 0;
  let y = spec.julia ? im : 0;
  const cx = spec.julia ? spec.juliaRe : re;
  const cy = spec.julia ? spec.juliaIm : im;
  let minimum = Number.POSITIVE_INFINITY;
  let maximum = 0;
  const componentEscape = TRANSCENDENTAL.has(spec.variant);
  const bailout2 = spec.bailout * spec.bailout;
  for (let iter = 0; iter < spec.iterations; iter += 1) {
    [x, y] = step(spec.variant, x, y, cx, cy);
    const norm = x * x + y * y;
    if (norm < minimum) minimum = norm;
    if (norm > maximum) maximum = norm;
    const escaped = !Number.isFinite(norm) || (componentEscape
      ? Math.max(Math.abs(x), Math.abs(y)) > spec.bailout
      : norm > bailout2);
    if (escaped) return { iter, norm, field: fieldValue(spec.metric, minimum, maximum) };
  }
  return { iter: spec.iterations, norm: 0, field: fieldValue(spec.metric, minimum, maximum) };
}

function fieldValue(metric: LocalMetric, minimum2: number, maximum2: number): number {
  const minimum = Number.isFinite(minimum2) ? Math.sqrt(minimum2) : 0;
  const maximum = maximum2 > 0 ? Math.sqrt(maximum2) : 0;
  if (metric === "min_abs") return minimum;
  if (metric === "max_abs") return maximum;
  if (metric === "envelope") return 0.5 * (minimum + maximum);
  return 0;
}

type RGB = [number, number, number];
const GRADIENTS: Record<string, ReadonlyArray<readonly [number, number, number, number]>> = {
  inferno: [[0,0,0,4],[.14,31,12,72],[.28,85,15,109],[.42,136,34,106],[.56,186,54,85],[.7,227,89,51],[.84,249,140,10],[.94,252,195,55],[1,252,255,164]],
  viridis: [[0,68,1,84],[.25,59,82,139],[.5,33,145,140],[.75,94,201,98],[1,253,231,37]],
  twilight: [[0,32,24,70],[.18,63,92,180],[.36,58,150,165],[.54,240,210,120],[.72,210,90,90],[.88,90,50,110],[1,32,24,70]],
  ember_blue: [[0,5,8,32],[.22,10,70,120],[.48,55,190,185],[.72,245,172,75],[1,255,246,210]],
};

const byte = (value: number) => Math.max(0, Math.min(255, Math.trunc(value)));

function gradient(name: string, value: number): RGB | null {
  const stops = GRADIENTS[name];
  if (!stops) return null;
  const t = Math.max(0, Math.min(1, value));
  for (let index = 1; index < stops.length; index += 1) {
    const left = stops[index - 1]; const right = stops[index];
    if (!left || !right || t > right[0]) continue;
    const u = (t - left[0]) / Math.max(1e-12, right[0] - left[0]);
    return [Math.round(left[1] * (1 - u) + right[1] * u), Math.round(left[2] * (1 - u) + right[2] * u), Math.round(left[3] * (1 - u) + right[3] * u)];
  }
  const last = stops[stops.length - 1]; return last ? [last[1], last[2], last[3]] : [0, 0, 0];
}

function hsv(hue: number): RGB {
  const h = ((hue % 360) + 360) % 360 / 60; const x = 1 - Math.abs(h % 2 - 1);
  const [r, g, b] = h < 1 ? [1,x,0] : h < 2 ? [x,1,0] : h < 3 ? [0,1,x] : h < 4 ? [0,x,1] : h < 5 ? [x,0,1] : [1,0,x];
  return [byte(r * 255), byte(g * 255), byte(b * 255)];
}

function hue1530(index: number): RGB {
  const i = ((Math.trunc(index) % 1530) + 1530) % 1530; const segment = Math.floor(i / 255); const d = i % 255;
  if (segment === 0) return [0,255,d]; if (segment === 1) return [0,255-d,255];
  if (segment === 2) return [d,0,255]; if (segment === 3) return [255,0,255-d];
  if (segment === 4) return [255,d,0]; return [255-d,255,0];
}

function tri765(index: number): RGB {
  const m = ((Math.trunc(index) % 765) + 765) % 765; const band = Math.floor(m / 255); const d = m % 255;
  return band === 0 ? [255-d,d,255] : band === 1 ? [d,255,255-d] : [255,255-d,d];
}

function rainbow1785(index: number): RGB {
  const i = Math.max(0, Math.min(1785, Math.trunc(index))); if (i === 0) return [0,0,0]; if (i === 1785) return [255,255,255];
  let blue = i; let red = 0; let green = 0;
  if (i > 255 && i < 510) { red=i-255; blue=510-i; } else if (i > 509 && i < 765) { red=255; blue=i-510; }
  else if (i > 764 && i < 1020) { green=i-765; red=1020-i; blue=red; } else if (i > 1019 && i < 1275) { green=255; blue=i-1020; }
  else if (i > 1274 && i < 1530) { green=255; red=i-1275; blue=1530-i; } else if (i > 1529) { green=255; red=255; blue=i-1530; }
  return [byte(red),byte(green),byte(blue)];
}

function fieldColor(value: number, palette: string): RGB {
  const t = Math.max(0, Math.min(1, Number.isFinite(value) ? value : 1));
  const g = gradient(palette, t); if (g) return g;
  if (palette === "grayscale") return [byte(t * 255), byte(t * 255), byte(t * 255)];
  if (palette === "hsv_wheel") return hsv(t * 360);
  if (palette === "tri765") return tri765(t * 765);
  if (palette === "hs_rainbow") return rainbow1785(t * 1785);
  if (palette === "spectral1530") return hue1530(Math.min(1529, t * 1530));
  if (palette === "mod17") { const v = Math.min(16, Math.trunc(t * 17)) * 15; return [v,v,v]; }
  const tau = Math.PI * 2;
  return [byte(128 - 128 * Math.cos(t * tau)), byte(128 - 128 * Math.cos(t * tau + 2.094395)), byte(128 - 128 * Math.cos(t * tau + 4.18879))];
}

function escapeColor(sample: OrbitSample, spec: LocalRenderSpec): RGB {
  if (sample.iter >= spec.iterations) return [255,255,255];
  let n = (sample.iter + 1) / (spec.iterations + 2);
  let mu = sample.iter;
  if (spec.smooth && sample.norm > 1) { mu = Math.max(0, sample.iter + 1 - Math.log2(Math.log2(sample.norm))); n = ((mu / 32) % 1 + 1) % 1; }
  const g = gradient(spec.colorMap, n); if (g) return g;
  if (spec.colorMap === "hsv_wheel") return hsv(spec.smooth ? n * 360 : (sample.iter % 1440) / 4);
  if (spec.colorMap === "tri765") return tri765(spec.smooth ? n * 765 : sample.iter);
  if (spec.colorMap === "grayscale") { const v = byte(n * 255); return [v,v,v]; }
  if (spec.colorMap === "spectral1530") return mu < 255 ? [0,byte(mu),0] : hue1530(mu - 255);
  if (spec.colorMap === "mod17") return spec.smooth ? (() => { const v = byte((Math.trunc(mu) % 17) * 15); return [v,v,v] as RGB; })() : [byte(sample.iter % 256), byte(sample.iter / 256), byte((sample.iter % 17) * 17)];
  return [byte(128 - 128 * Math.cos(n * 53 * Math.PI)), byte(128 - 128 * Math.cos(n * 27 * Math.PI)), byte(128 - 128 * Math.cos(n * 139 * Math.PI))];
}

function metricColor(sample: OrbitSample, spec: LocalRenderSpec): RGB {
  const raw = sample.field;
  if (spec.colorMap === "hs_rainbow") {
    if (raw <= 0 || !Number.isFinite(raw)) return [255,255,255];
    return rainbow1785((36 / 35 - Math.log2(raw)) * 35);
  }
  if (spec.smooth) {
    if (raw <= 0) return [255,255,255];
    const base = 2 - Math.log2(raw); let cycle = ((base / 8) % 1 + 1) % 1;
    const g = gradient(spec.colorMap, cycle); if (g) return g;
    if (spec.colorMap === "hsv_wheel") return hsv(Math.max(0, Math.trunc(180 * base)) % 1440 / 4);
    if (spec.colorMap === "tri765") return tri765(Math.max(0, Math.trunc(96 * base)));
    if (spec.colorMap === "spectral1530") return hue1530(Math.max(0, Math.trunc(191 * base)));
    if (spec.colorMap === "grayscale") { const value = Math.max(0, Math.trunc(32 * base)) % 256; return [value,value,value]; }
  }
  return fieldColor(raw / spec.bailout, spec.colorMap);
}

export function renderLocalRgba(spec: LocalRenderSpec, width: number, height: number): Uint8ClampedArray {
  const count = width * height; const iterations = new Uint32Array(count); const norms = new Float64Array(count);
  const fields = spec.metric === "escape" ? null : new Float64Array(count); const histogram = new Float64Array(spec.iterations);
  const aspect = width / height; const angle = spec.rotationDeg * Math.PI / 180; const cosine = Math.cos(angle); const sine = Math.sin(angle);
  let total = 0;
  for (let y = 0; y < height; y += 1) for (let x = 0; x < width; x += 1) {
    const localRe = ((x + 0.5) / width - 0.5) * spec.scale * aspect;
    const localIm = (0.5 - (y + 0.5) / height) * spec.scale;
    const re = spec.centerRe + localRe * cosine - localIm * sine; const im = spec.centerIm + localRe * sine + localIm * cosine;
    const sample = iterateOrbit(spec, re, im); const sampleIndex = y * width + x;
    iterations[sampleIndex] = sample.iter; norms[sampleIndex] = sample.norm; if (fields) fields[sampleIndex] = sample.field;
    if (spec.metric === "escape" && sample.iter < spec.iterations && spec.colorMode !== "direct") {
      if (spec.colorMode === "eq_center" && re * re + im * im > 4) continue;
      const weight = spec.colorMode === "eq_center" ? 1 / Math.max((re-spec.centerRe) ** 2 + (im-spec.centerIm) ** 2, (spec.scale / height) ** 2) : 1;
      histogram[sample.iter] = (histogram[sample.iter] ?? 0) + weight; total += weight;
    }
  }
  let countMin = 0; while (countMin < histogram.length && !histogram[countMin]) countMin += 1;
  let median = countMin; let cumulative = 0;
  for (let i = 0; i < histogram.length; i += 1) { cumulative += histogram[i] ?? 0; if (cumulative >= total / 2) { median = i; break; } }
  const countMax = Math.max(countMin + 1, countMin + 2 * (median - countMin));
  const totalOctaves = Math.max(1, Math.log2(4 / Math.max(1e-300, spec.scale)));
  const period = Math.max(1e-6, Math.max(1, countMax - countMin) / Math.max(1e-6, totalOctaves * spec.cyclesPerOctave));
  const wraps = ["classic_cos", "hsv_wheel", "tri765", "twilight", "spectral1530"].includes(spec.colorMap);
  const equalizedColor = (iteration: number): RGB => {
    const phase = Math.max(0, (iteration - countMin) / period); const onset = 1 / 6;
    if (phase < onset) { let value = phase / onset; value = value * value * (3 - 2 * value); const start = fieldColor(0, spec.colorMap); return start.map((channel) => Math.round(channel * value)) as RGB; }
    let value = phase - onset; value -= Math.floor(value); if (!wraps) value = 1 - Math.abs(2 * value - 1);
    return fieldColor(value, spec.colorMap);
  };
  const rgba = new Uint8ClampedArray(count * 4);
  for (let index = 0; index < count; index += 1) {
    const sample = { iter: iterations[index] ?? spec.iterations, norm: norms[index] ?? 0, field: fields?.[index] ?? 0 };
    const rgb = spec.metric !== "escape" ? metricColor(sample, spec)
      : spec.colorMode !== "direct" && sample.iter < spec.iterations && total > 0 ? equalizedColor(sample.iter) : escapeColor(sample, spec);
    const offset = index * 4; rgba[offset] = rgb[0]; rgba[offset+1] = rgb[1]; rgba[offset+2] = rgb[2]; rgba[offset+3] = 255;
  }
  return rgba;
}
