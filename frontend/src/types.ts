// Shared types across the frontend.

export interface StatusState {
  cpu: number | null
  gpu: number | null
  renderMs: number | null
  engine: string    // "openmp·fp64", "avx512·fx64", "cuda·fp64", etc.
  scalar: string    // "fp32", "fp64", "fp80", "fp128", "fx64", or a
                    // perturbation combo like "perturb-mpfr192-fp32"
                    // (reference orbit precision + per-pixel delta precision)
  cRe: number | null
  cIm: number | null
  zoom: number | null
  iter: number | null
  variant: string
  metric: string
  message: string
}
