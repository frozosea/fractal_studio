// Run and artifact types — run history, active tasks, progress, resource locks.

import type { RunStatus, LnMapColorMode } from './api';

export { type RunStatus };

export interface RunRow {
  id: string;
  module: string;
  status: string;
  startedAt: number;
  finishedAt: number;
  outputDir: string;
  cancelable?: boolean;
  cancelRequested?: boolean;
}

export interface RunProgress {
  stage?: string;
  current?: number;
  total?: number;
  percent?: number;
  engine?: string;
  scalar?: string;
  elapsedMs?: number;
  estimatedRemainingMs?: number | null;
  cancelable?: boolean;
  resourceLocks?: string[];
  depthOctave?: number;
  totalDepthOctaves?: number;
  currentFrame?: number;
  totalFrames?: number;
  currentLnMapRow?: number;
  totalLnMapRows?: number;
  currentLnMapSegment?: number;
  lnMapSegmentCount?: number;
  lnMapSegmentHeight?: number;
  finalFrameEngine?: string;
  finalFrameScalar?: string;
  lnMapEngine?: string;
  lnMapScalar?: string;
  lnMapMode?: string;
  lnMapColorMode?: LnMapColorMode;
  lnMapPass?: 'equalization' | 'render' | string;
  lnMapStatsSource?: string;
  lnMapStatsReused?: boolean;
  lnMapCyclesPerOctave?: number;
  lnMapLayerSummary?: string;
  lnMapValidationSummary?: string;
  warpMethod?: string;
  encoder?: string;
  failedStage?: string;
  errorMessage?: string;
  details?: Record<string, any>;
}

export interface RunArtifactStatus {
  artifactId: string;
  name: string;
  kind: string;
  downloadUrl: string;
  contentUrl: string;
  localPath?: string;
}

export interface RunStatusResponse {
  id: string;
  module: string;
  status: string;
  startedAt: number;
  finishedAt: number;
  outputDir: string;
  cancelRequested?: boolean;
  progress: RunProgress;
  artifacts: RunArtifactStatus[];
}

export interface ActiveTask {
  runId: string;
  taskType: string;
  status: string;
  stage: string;
  engine?: string;
  scalar?: string;
  startedAt: number;
  elapsedMs: number;
  cancelable: boolean;
  cancelRequested: boolean;
  progress: RunProgress;
}

export interface ActiveTasksResponse {
  items: ActiveTask[];
  resourceLocks: ResourceLockStatus[];
}

export interface ResourceLockStatus {
  name: string;
  active: number;
  limit: number;
  busy: boolean;
  activeRunId?: string;
  taskType?: string;
}

export interface ArtifactRow {
  artifactId: string;
  runId: string;
  name: string;
  kind: string;
  sizeBytes: number;
  downloadPath: string;
  contentPath: string;
  localPath?: string;
}
