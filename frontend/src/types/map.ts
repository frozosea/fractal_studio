// Map rendering types — mirror the fractal_studio C++ backend's
// MapRenderRequest/Response, MapRenderInlineResponse, MapFieldRequest/Response,
// MapFieldSessionStartRequest/Status/Snapshot/Result, MapPreemptRequest/Response,
// LnMapRequest/Response.

import type { Variant, Metric, ColorMap, LnMapColorMode } from './api';

export interface TransitionLegInput {
  variant: Variant | string;
  weight: number;
}

export interface MapRenderRequest {
  requestId?: string;
  preemptKey?: string;
  preemptSeq?: number;
  taskType?: string;
  localExport?: boolean;
  background?: boolean;
  centerRe: number;
  centerIm: number;
  centerReStr?: string;
  centerImStr?: string;
  scale: number;
  viewportAspect?: number;
  width: number;
  height: number;
  iterations: number;
  variant?: Variant | string;
  metric?: Metric;
  colorMap?: ColorMap;
  smooth?: boolean;
  colorMode?: 'direct' | 'eq_full' | 'eq_center';
  cyclesPerOctave?: number;
  bailout?: number;
  bailoutSq?: number;
  pairwiseCap?: number;
  julia?: boolean;
  juliaRe?: number;
  juliaIm?: number;
  transitionTheta?: number;
  transitionThetaMilliDeg?: number;
  transitionFrom?: Variant | string;
  transitionTo?: Variant | string;
  transitionVariants?: Array<Variant | string>;
  transitionWeights?: number[];
  transitionLegs?: TransitionLegInput[];
  engine?: string;
  scalarType?: string;
  rotationDeg?: number;
}

export interface MapRenderResponse {
  runId: string;
  requestId?: string;
  status: string;
  artifactId: string;
  imagePath: string;
  localPath?: string;
  localExport?: boolean;
  generatedMs: number;
  width: number;
  height: number;
  effective: Record<string, any>;
}

export interface MapRenderInlineResponse {
  status: string;
  requestId?: string;
  data?: ArrayBuffer;
  generatedMs: number;
  width: number;
  height: number;
  engineUsed?: string;
  scalarUsed?: string;
  pixelFormat?: string;
}

export interface MapPreemptRequest {
  preemptKey: string;
  preemptSeq: number;
}

export interface MapPreemptResponse {
  status: string;
  preemptKey?: string;
  preemptSeq?: number;
}

export interface MapFieldRequest {
  requestId?: string;
  preemptKey?: string;
  preemptSeq?: number;
  centerRe: number;
  centerIm: number;
  centerReStr?: string;
  centerImStr?: string;
  scale: number;
  viewportAspect?: number;
  width: number;
  height: number;
  iterations: number;
  variant?: Variant | string;
  metric?: Metric;
  bailout?: number;
  bailoutSq?: number;
  pairwiseCap?: number;
  julia?: boolean;
  juliaRe?: number;
  juliaIm?: number;
  engine?: string;
  scalarType?: string;
  rotationDeg?: number;
}

export interface MapFieldResponse {
  status: string;
  requestId?: string;
  width: number;
  height: number;
  viewportAspect?: number;
  metric: string;
  maxIter?: number;
  iterB64?: string;
  finalMagB64?: string;
  fieldB64?: string;
  fieldMin?: number;
  fieldMax?: number;
  generatedMs: number;
  scalarUsed?: string;
  engineUsed?: string;
}

export interface MapFieldSessionStartRequest extends MapFieldRequest {
  colorMap?: ColorMap;
  smooth?: boolean;
  colorMode?: 'direct' | 'eq_full' | 'eq_center';
  slowAfterMs?: number;
}

export type MapFieldSessionState = 'running' | 'completed' | 'cancelled' | 'failed';

export interface MapFieldSessionStatus {
  sessionId?: string;
  requestId?: string;
  status: MapFieldSessionState | string;
  state: MapFieldSessionState | string;
  width?: number;
  height?: number;
  viewportAspect?: number;
  centerRe?: number;
  centerIm?: number;
  scale?: number;
  rotationDeg?: number;
  elapsedMs?: number;
  slowAfterMs?: number;
  deadlinePassed?: boolean;
  presentationPhase?: 'native_wait' | 'degraded' | 'full' | string;
  revision?: number;
  completedPixels?: number;
  totalPixels?: number;
  coverage?: number;
  generatedMs?: number;
  scalarUsed?: string;
  engineUsed?: string;
  error?: string;
  started?: boolean;
  resultAcknowledged?: boolean;
}

export interface MapFieldSessionSnapshot extends MapFieldSessionStatus {
  previewWidth?: number;
  previewHeight?: number;
  previewAvailable?: boolean;
  rgbaB64?: string;
}

export interface MapFieldSessionResult extends Partial<MapFieldResponse> {
  sessionId?: string;
  state?: MapFieldSessionState | string;
  status: string;
  error?: string;
}

export interface LnMapRequest {
  centerRe: number;
  centerIm: number;
  centerReStr?: string;
  centerImStr?: string;
  julia?: boolean;
  juliaRe?: number;
  juliaIm?: number;
  widthS?: number;
  width?: number;
  height?: number;
  depthOctaves: number;
  qualityPreset?: 'draft' | 'balanced' | 'high' | 'full' | 'custom';
  qualityScale?: number;
  lnMapExtraOctaves?: number;
  variant?: Variant;
  colorMap?: ColorMap;
  lnMapColorMode?: LnMapColorMode;
  lnMapCyclesPerOctave?: number;
  iterations?: number;
  engine?: string;
  precisionMode?: 'standard' | 'fast';
  scalarType?: string;
  fastFp32DepthOctaves?: number;
  fastFp64DepthOctaves?: number;
  fastValidate?: boolean;
  fastValidationBandOctaves?: number;
  fastValidationSampleRows?: number;
  fastValidationSampleCols?: number;
  fastValidationMaxMismatchRatio?: number;
  fastValidationMaxP99IterDelta?: number;
  fastValidationMaxMeanColorDelta?: number;
}

export interface LnMapResponse {
  runId: string;
  status: string;
  artifactId: string;
  imagePath: string;
  widthS: number;
  heightT: number;
  depthOctaves: number;
  engineUsed?: string;
  scalarUsed?: string;
  precisionMode?: string;
  lnMapColorMode?: LnMapColorMode;
  lnMapCyclesPerOctave?: number;
  layerSummary?: string;
  validationSummary?: string;
  generatedMs: number;
}
