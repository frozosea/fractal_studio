// Mesh rendering types — hidden structure (Hs) and transition meshes/voxels.

import type { Variant } from './api';

export type HsStage = 'min_abs' | 'max_abs' | 'envelope' | 'min_pairwise_dist';

export interface HsMeshRequest {
  centerRe?: number;
  centerIm?: number;
  scale?: number;
  width?: number;
  height?: number;
  resolution?: number;
  metric?: HsStage;
  variant?: Variant;
  iterations?: number;
  heightScale?: number;
  pairwiseCap?: number;
}

export interface HsFieldRequest {
  centerRe?: number;
  centerIm?: number;
  scale?: number;
  resolution?: number;
  metric?: HsStage;
  variant?: Variant;
  iterations?: number;
  bailout?: number;
  bailoutSq?: number;
  heightClamp?: number;
  pairwiseCap?: number;
}

export interface HsFieldResponse {
  runId: string;
  status: string;
  width: number;
  height: number;
  fieldMin: number;
  fieldMax: number;
  fieldB64: string;
  generatedMs: number;
}

export interface MeshResponse {
  runId: string;
  status: string;
  glbArtifactId: string;
  glbUrl: string;
  stlArtifactId: string;
  stlUrl: string;
  vertexCount: number;
  triangleCount: number;
  generatedMs?: number;
  fieldMs?: number;
  mcMs?: number;
}

export interface TransitionMeshRequest {
  centerX?: number;
  centerY?: number;
  centerZ?: number;
  extent?: number;
  resolution?: number;
  iso?: number;
  iterations?: number;
  bailout?: number;
  bailoutSq?: number;
  transitionFrom?: Variant | string;
  transitionTo?: Variant | string;
  transitionVariants?: Array<Variant | string>;
  transitionWeights?: number[];
  transitionLegs?: TransitionLegInput[];
  engine?: string;
}

export interface TransitionVoxelRequest {
  centerX?: number;
  centerY?: number;
  centerZ?: number;
  extent?: number;
  resolution?: number;
  iso?: number;
  iterations?: number;
  bailout?: number;
  bailoutSq?: number;
  transitionFrom?: Variant | string;
  transitionTo?: Variant | string;
  transitionVariants?: Array<Variant | string>;
  transitionWeights?: number[];
  transitionLegs?: TransitionLegInput[];
  engine?: string;
}

export interface TransitionVoxelResponse {
  runId: string;
  status: string;
  resolution: number;
  extent: number;
  voxelCount?: number;
  faceCount: number;
  generatedMs: number;
  stlArtifactId?: string;
  stlUrl?: string;
  posB64: string;
  normB64: string;
  depthB64: string;
}

// Re-export TransitionLegInput from map types since mesh types also use it.
import type { TransitionLegInput } from './map';
export type { TransitionLegInput };
