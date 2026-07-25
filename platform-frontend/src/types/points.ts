// Special point types — enumeration, search, snapping, and orbit classification.

import type { SpecialPointKind } from './api';

export interface SpecialPoint {
  id: string;
  family: string;
  pointType: string;
  k: number;
  p: number;
  real: number;
  imag: number;
  sourceMode: string;
  createdAt: string;
}

export { type SpecialPointKind };

export interface SpecialPointViewport {
  centerRe: number;
  centerIm: number;
  centerReStr?: string;
  centerImStr?: string;
  scale: number;
  width: number;
  height: number;
  rotationDeg?: number;
}

export interface SpecialPointEnumRequest {
  kind: SpecialPointKind;
  periodMin?: number;
  periodMax?: number;
  preperiodMin?: number;
  preperiodMax?: number;
  maxNewtonIter?: number;
  maxSeedBatches?: number;
  seedsPerBatch?: number;
  includeVariantExistence?: boolean;
  includeRejectedDebug?: boolean;
  visibleOnly?: boolean;
  viewport?: SpecialPointViewport;
}

export interface SpecialPointSearchRequest {
  preemptKey?: string;
  preemptSeq?: number;
  kind?: SpecialPointKind;
  periodMin?: number;
  periodMax?: number;
  preperiodMin?: number;
  preperiodMax?: number;
  seedBudget?: number;
  maxNewtonIter?: number;
  includeVariantCompatibility?: boolean;
  visibleOnly?: boolean;
  viewport: SpecialPointViewport;
}

export interface SpecialPointSnapRequest {
  period: number;
  re: number;
  im: number;
  maxNewtonIter?: number;
  includeVariantCompatibility?: boolean;
}

export interface SpecialPointEnumResult {
  id: string;
  kind: SpecialPointKind;
  preperiod: number;
  period: number;
  re: number;
  im: number;
  reStr?: string;
  imStr?: string;
  precBits?: number;
  offsetRe?: number;
  offsetIm?: number;
  real?: number;
  imag?: number;
  converged: boolean;
  success?: boolean;
  accepted: boolean;
  fallback?: boolean;
  visible: boolean;
  residual: number;
  newtonIterations: number;
  actual: OrbitClassification;
  variants: VariantExistence[];
  compatibleVariants?: string[];
  variantCompatibility?: Record<string, any>;
  reason: string;
}

export interface SpecialPointEnumResponse {
  runId: string;
  complete: boolean;
  status: string;
  acceptedCount: number;
  expectedCount: number;
  seedCount: number;
  newtonSuccessCount: number;
  rejectedCount: number;
  points: SpecialPointEnumResult[];
  rejected_debug?: SpecialPointEnumResult[];
  warning?: string;
  reportArtifactId?: string;
  reportDownloadUrl?: string;
}

export interface SpecialPointSearchResponse {
  runId: string;
  status: string;
  sampled: boolean;
  foundAny?: boolean;
  noPoint?: boolean;
  acceptedCount: number;
  fallbackCount?: number;
  seedCount: number;
  newtonSuccessCount: number;
  rejectedCount: number;
  points: SpecialPointEnumResult[];
  warning?: string;
  reportArtifactId?: string;
  reportDownloadUrl?: string;
}

export interface OrbitClassification {
  kind?: string;
  found_repeat: boolean;
  is_center: boolean;
  is_misiurewicz: boolean;
  preperiod: number;
  period: number;
  repeat_error: number;
}

export interface VariantExistence {
  variant_name: string;
  exists: boolean;
  same_orbit_as_mandelbrot: boolean;
  actual_preperiod: number;
  actual_period: number;
  repeat_error: number;
  reason: string;
}
