// Video export types — zoom videos, transition videos, and previews.

import type { Variant, ColorMap, LnMapColorMode, TransitionVideoMode } from './api';

export { type TransitionVideoMode };

export interface VideoExportRequest {
  centerRe: number;
  centerIm: number;
  centerReStr?: string;
  centerImStr?: string;
  julia?: boolean;
  juliaRe?: number;
  juliaIm?: number;
  variant?: Variant | string;
  colorMap?: ColorMap;
  iterations?: number;
  bailout?: number;
  bailoutSq?: number;
  widthS?: number;
  depthOctaves?: number;
  fps?: number;
  secondsPerOctave?: number;
  durationSec?: number;
  targetScale?: number;
  qualityPreset?: 'draft' | 'balanced' | 'high' | 'full' | 'custom';
  qualityScale?: number;
  lnMapEngine?: string;
  lnMapMode?: 'standard' | 'fast';
  lnMapScalar?: string;
  lnMapColorMode?: LnMapColorMode;
  lnMapCyclesPerOctave?: number;
  lnMapFastValidate?: boolean;
  lnMapFastValidationBandOctaves?: number;
  lnMapFastValidationSampleRows?: number;
  lnMapFastValidationSampleCols?: number;
  lnMapFastValidationMaxMismatchRatio?: number;
  lnMapFastValidationMaxP99IterDelta?: number;
  lnMapFastValidationMaxMeanColorDelta?: number;
  lnMapExtraOctaves?: number;
  lnMapMaxSegmentHeight?: number;
  lnMapRunId?: string;
  lnMapStatsRunId?: string;
  lnMapPreviewRunId?: string;
  cudaWarp?: boolean;
  background?: boolean;
  localExport?: boolean;
  width?: number;
  height?: number;
  rotationDeg?: number;
}

export interface VideoExportResponse {
  runId: string;
  status: string;
  videoArtifactId?: string;
  videoUrl?: string;
  videoDownloadUrl?: string;
  videoLocalPath?: string;
  lnMapArtifactId?: string;
  lnMapDownloadUrl?: string;
  lnMapLocalPath?: string;
  finalFrameArtifactId?: string;
  finalFrameDownloadUrl?: string;
  finalFrameLocalPath?: string;
  startFrameArtifactId?: string;
  startFrameUrl?: string;
  startFrameDownloadUrl?: string;
  startFrameLocalPath?: string;
  endFrameArtifactId?: string;
  endFrameUrl?: string;
  endFrameDownloadUrl?: string;
  endFrameLocalPath?: string;
  reportArtifactId?: string;
  reportDownloadUrl?: string;
  reportLocalPath?: string;
  localExport?: boolean;
  frameCount: number;
  fps: number;
  durationSec: number;
  secondsPerOctave?: number;
  depthOctaves?: number;
  targetScale?: number;
  rotationDeg?: number;
  fullWidthS?: number;
  actualWidthS?: number;
  heightT?: number;
  lnMapExtraOctaves?: number;
  lnMapSegmented?: boolean;
  lnMapSegmentCount?: number;
  lnMapMaxSegmentHeight?: number;
  lnMapTotalSegmentRows?: number;
  qualityPreset?: string;
  qualityScale?: number;
  estimatedPeakMemory?: number;
  estimatedSingleStripMemory?: number;
  finalFrameEngine?: string;
  finalFrameScalar?: string;
  lnMapEngine?: string;
  lnMapScalar?: string;
  lnMapMode?: string;
  lnMapColorMode?: LnMapColorMode;
  lnMapStatsSource?: string;
  lnMapStatsReused?: boolean;
  lnMapCyclesPerOctave?: number;
  lnMapLayerSummary?: string;
  lnMapValidationSummary?: string;
  warpMethod?: string;
  encoder?: string;
  ffmpegStderr?: string;
  width: number;
  height: number;
  generatedMs?: number;
}

export interface VideoPreviewRequest extends VideoExportRequest {
  previewWidth?: number;
  previewHeight?: number;
}

export interface VideoPreviewResponse {
  runId: string;
  status: string;
  startFrameArtifactId: string;
  startFrameUrl: string;
  startFrameDownloadUrl: string;
  endFrameArtifactId: string;
  endFrameUrl: string;
  endFrameDownloadUrl: string;
  frameCount: number;
  fps: number;
  durationSec: number;
  secondsPerOctave: number;
  depthOctaves: number;
  targetScale: number;
  rotationDeg?: number;
  width: number;
  height: number;
  outputWidth: number;
  outputHeight: number;
  actualWidthS?: number;
  heightT?: number;
  lnMapArtifactId?: string;
  lnMapDownloadUrl?: string;
  finalFrameArtifactId?: string;
  finalFrameDownloadUrl?: string;
  lnMapColorMode?: LnMapColorMode;
  lnMapStatsSource?: string;
  lnMapCyclesPerOctave?: number;
  lnMapEngine?: string;
  lnMapScalar?: string;
  lnMapMode?: string;
  generatedMs: number;
}

export interface VideoZoomRequest {
  lnMapArtifactId: string;
  localExport?: boolean;
  fps?: number;
  durationSec?: number;
  secondsPerOctave?: number;
  targetScale?: number;
  width?: number;
  height?: number;
  startLnRadius?: number;
  depthOctaves?: number;
  cudaWarp?: boolean;
  rotationDeg?: number;
}

export interface VideoZoomResponse {
  runId: string;
  status: string;
  artifactId: string;
  videoUrl: string;
  downloadUrl: string;
  localPath?: string;
  localExport?: boolean;
  frameCount: number;
  fps: number;
  durationSec: number;
  secondsPerOctave?: number;
  depthOctaves?: number;
  rotationDeg?: number;
  width: number;
  height: number;
  warpMethod?: string;
  encoder?: string;
  ffmpegStderr?: string;
  generatedMs: number;
}

export interface TransitionVideoExportRequest {
  animationMode?: TransitionVideoMode;
  centerRe: number;
  centerIm: number;
  centerReStr?: string;
  centerImStr?: string;
  julia?: boolean;
  juliaRe?: number;
  juliaIm?: number;
  transitionFrom?: Variant | string;
  transitionTo?: Variant | string;
  colorMap?: ColorMap;
  iterations?: number;
  bailout?: number;
  bailoutSq?: number;
  scale?: number;
  thetaStartDeg?: number;
  thetaEndDeg?: number;
  thetaDeg?: number;
  depthOctaves?: number;
  secondsPerOctave?: number;
  targetScale?: number;
  rotationDeg?: number;
  durationSec?: number;
  fps?: number;
  metric?: string;
  engine?: string;
  scalarType?: string;
  background?: boolean;
  localExport?: boolean;
  width?: number;
  height?: number;
}

export interface TransitionVideoExportResponse {
  runId: string;
  status: string;
  videoArtifactId?: string;
  videoUrl?: string;
  videoDownloadUrl?: string;
  videoLocalPath?: string;
  startFrameArtifactId?: string;
  startFrameUrl?: string;
  startFrameDownloadUrl?: string;
  endFrameArtifactId?: string;
  endFrameUrl?: string;
  endFrameDownloadUrl?: string;
  reportArtifactId?: string;
  reportDownloadUrl?: string;
  localExport?: boolean;
  frameCount: number;
  fps: number;
  durationSec: number;
  thetaStartDeg: number;
  thetaEndDeg: number;
  rotationDeg?: number;
  transitionFrom: string;
  transitionTo: string;
  width: number;
  height: number;
  engine?: string;
  scalarType?: string;
  encoder?: string;
  ffmpegStderr?: string;
  generatedMs?: number;
  animationMode?: TransitionVideoMode;
  thetaDeg?: number;
  depthOctaves?: number;
  targetScale?: number;
  secondsPerOctave?: number;
}

export interface TransitionVideoPreviewRequest extends TransitionVideoExportRequest {
  previewWidth?: number;
  previewHeight?: number;
}

export interface TransitionVideoPreviewResponse {
  runId: string;
  status: string;
  startFrameArtifactId: string;
  startFrameUrl: string;
  startFrameDownloadUrl: string;
  endFrameArtifactId: string;
  endFrameUrl: string;
  endFrameDownloadUrl: string;
  thetaStartDeg: number;
  thetaEndDeg: number;
  rotationDeg?: number;
  width: number;
  height: number;
  outputWidth: number;
  outputHeight: number;
  generatedMs: number;
  animationMode?: TransitionVideoMode;
  thetaDeg?: number;
  frameCount?: number;
  fps?: number;
  durationSec?: number;
  depthOctaves?: number;
  targetScale?: number;
  secondsPerOctave?: number;
}
