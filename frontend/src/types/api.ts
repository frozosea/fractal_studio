// API response envelopes and shared enums

export interface ApiResponse<T> {
  data: T;
}

export interface PaginatedResponse<T> {
  items: T[];
  totalCount: number;
  modules: string[];
}

// ── Variants ──────────────────────────────────────────────────────────────────

export type Variant =
  | 'mandelbrot'
  | 'tricorn'
  | 'burning_ship'
  | 'celtic'
  | 'heart'
  | 'buffalo'
  | 'perp_buffalo'
  | 'celtic_ship'
  | 'mandelceltic'
  | 'perp_ship'
  | 'sin_z'
  | 'cos_z'
  | 'exp_z'
  | 'sinh_z'
  | 'cosh_z'
  | 'tan_z';

export const VARIANTS: Variant[] = [
  'mandelbrot',
  'tricorn',
  'burning_ship',
  'celtic',
  'heart',
  'buffalo',
  'perp_buffalo',
  'celtic_ship',
  'mandelceltic',
  'perp_ship',
  'sin_z',
  'cos_z',
  'exp_z',
  'sinh_z',
  'cosh_z',
  'tan_z',
];

export const VARIANT_LABELS: Record<Variant, { en: string; zh: string }> = {
  mandelbrot: { en: 'Mandelbrot', zh: '曼德布罗特' },
  tricorn: { en: 'Tricorn / Mandelbar', zh: '三角帽 Tricorn' },
  burning_ship: { en: 'Burning Ship', zh: '燃烧船 Burning Ship' },
  celtic: { en: 'Perpendicular Burning Ship', zh: '垂直燃烧船' },
  heart: { en: 'Perpendicular Mandelbrot', zh: '垂直曼德布罗特' },
  buffalo: { en: 'Celtic', zh: '凯尔特 Celtic' },
  perp_buffalo: { en: 'Mandelbar Celtic', zh: '曼德尔巴凯尔特' },
  celtic_ship: { en: 'Buffalo', zh: '水牛 Buffalo' },
  mandelceltic: { en: 'Perpendicular Buffalo', zh: '垂直水牛' },
  perp_ship: { en: 'Perpendicular Celtic', zh: '垂直凯尔特' },
  sin_z: { en: 'sin(z)+c', zh: 'sin(z)+c' },
  cos_z: { en: 'cos(z)+c', zh: 'cos(z)+c' },
  exp_z: { en: 'exp(z)+c', zh: 'exp(z)+c' },
  sinh_z: { en: 'sinh(z)+c', zh: 'sinh(z)+c' },
  cosh_z: { en: 'cosh(z)+c', zh: 'cosh(z)+c' },
  tan_z: { en: 'tan(z)+c', zh: 'tan(z)+c' },
};

// ── Metrics ───────────────────────────────────────────────────────────────────

export type Metric =
  | 'escape'
  | 'min_abs'
  | 'max_abs'
  | 'envelope'
  | 'min_pairwise_dist'
  | 'mandel_ship_agree';

export const METRICS: Metric[] = [
  'escape',
  'min_abs',
  'max_abs',
  'envelope',
  'min_pairwise_dist',
  'mandel_ship_agree',
];

// ── Color Maps ────────────────────────────────────────────────────────────────

export type ColorMap =
  | 'classic_cos'
  | 'mod17'
  | 'hsv_wheel'
  | 'tri765'
  | 'grayscale'
  | 'hs_rainbow'
  | 'inferno'
  | 'viridis'
  | 'twilight'
  | 'ember_blue'
  | 'spectral1530';

export const COLORMAPS: ColorMap[] = [
  'classic_cos',
  'mod17',
  'hsv_wheel',
  'tri765',
  'grayscale',
  'hs_rainbow',
  'inferno',
  'viridis',
  'twilight',
  'ember_blue',
  'spectral1530',
];

// ── LnMap Color Mode ──────────────────────────────────────────────────────────

export type LnMapColorMode =
  | 'escape'
  | 'hist_eq'
  | 'row_eq'
  | 'log_lift'
  | 'bands'
  | 'frontier';

// ── Run Status ────────────────────────────────────────────────────────────────

export type RunStatus =
  | 'pending'
  | 'running'
  | 'completed'
  | 'failed'
  | 'cancelled'
  | 'cancelling';

// ── Special Point Kind ────────────────────────────────────────────────────────

export type SpecialPointKind = 'center' | 'misiurewicz';

// ── Transition Video Mode ─────────────────────────────────────────────────────

export type TransitionVideoMode = 'rotation' | 'zoom';
