// api.ts — typed client for the native fractal_studio backend.
//
// All endpoints are POST JSON / GET query-string. Returns parsed JSON.

const viteEnv = (import.meta as any).env
const backendPort = viteEnv?.VITE_BACKEND_PORT ?? '18080'
const BASE =
  viteEnv?.VITE_BACKEND_URL ??
  `http://${location.hostname}:${backendPort}`

function isLoopbackHostname(hostname: string): boolean {
  const host = hostname.toLowerCase().replace(/^\[|\]$/g, '')
  if (host === 'localhost' || host === '::1') return true
  const octets = host.split('.')
  return octets.length === 4 && octets[0] === '127' &&
    octets.every(part => /^\d{1,3}$/.test(part) && Number(part) <= 255)
}

export function isLocalBrowserAccess(): boolean {
  if (!isLoopbackHostname(location.hostname)) return false
  try {
    // Local export writes to the backend machine. A local Vite page pointed at
    // a remote VITE_BACKEND_URL must therefore keep download-based export.
    return isLoopbackHostname(new URL(BASE, location.href).hostname)
  } catch {
    return false
  }
}

async function postJson<T>(path: string, body: unknown, signal?: AbortSignal): Promise<T> {
  const res = await fetch(BASE + path, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
    signal,
  })
  const text = await res.text()
  if (!res.ok) {
    const err = new Error(`${path}: ${res.status} ${text}`) as Error & { status?: number; data?: any }
    err.status = res.status
    try { err.data = JSON.parse(text) } catch {}
    throw err
  }
  return (text ? JSON.parse(text) : {}) as T
}

async function getJson<T>(path: string): Promise<T> {
  const res = await fetch(BASE + path)
  if (!res.ok) throw new Error(`${path}: ${res.status}`)
  return res.json() as Promise<T>
}

async function postArrayBuffer(path: string, body: unknown, signal?: AbortSignal): Promise<{ status: number; headers: Headers; data?: ArrayBuffer }> {
  const res = await fetch(BASE + path, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
    signal,
  })
  if (res.status === 204) {
    return { status: res.status, headers: res.headers }
  }
  if (!res.ok) {
    const text = await res.text()
    const err = new Error(`${path}: ${res.status} ${text}`) as Error & { status?: number; data?: any }
    err.status = res.status
    try { err.data = JSON.parse(text) } catch {}
    throw err
  }
  return { status: res.status, headers: res.headers, data: await res.arrayBuffer() }
}

function numberHeader(headers: Headers, name: string, fallback = 0): number {
  const raw = headers.get(name)
  if (raw === null || raw === '') return fallback
  const value = Number(raw)
  return Number.isFinite(value) ? value : fallback
}

// ---- Types ----

export type Variant =
  | 'mandelbrot' | 'tricorn' | 'burning_ship' | 'celtic' | 'heart'
  | 'buffalo' | 'perp_buffalo' | 'celtic_ship' | 'mandelceltic' | 'perp_ship'
  | 'sin_z' | 'cos_z' | 'exp_z' | 'sinh_z' | 'cosh_z' | 'tan_z'

export const VARIANTS: Variant[] = [
  'mandelbrot', 'tricorn', 'burning_ship', 'celtic', 'heart',
  'buffalo', 'perp_buffalo', 'celtic_ship', 'mandelceltic', 'perp_ship',
  'sin_z', 'cos_z', 'exp_z', 'sinh_z', 'cosh_z', 'tan_z',
]

// Human-readable display names, used in dropdowns and info panels
export const VARIANT_LABELS: Record<Variant, { en: string; zh: string }> = {
  mandelbrot:   { en: 'Mandelbrot',                 zh: '曼德布罗特 Mandelbrot' },
  tricorn:      { en: 'Tricorn / Mandelbar',        zh: '三角帽 Tricorn' },
  burning_ship: { en: 'Burning Ship',               zh: '燃烧船 Burning Ship' },
  celtic:       { en: 'Perpendicular Burning Ship', zh: '垂直燃烧船' },
  heart:        { en: 'Perpendicular Mandelbrot',   zh: '垂直曼德布罗特' },
  buffalo:      { en: 'Celtic',                     zh: '凯尔特 Celtic' },
  perp_buffalo: { en: 'Mandelbar Celtic',           zh: '曼德尔巴凯尔特' },
  celtic_ship:  { en: 'Buffalo',                    zh: '水牛 Buffalo' },
  mandelceltic: { en: 'Perpendicular Buffalo',      zh: '垂直水牛' },
  perp_ship:    { en: 'Perpendicular Celtic',       zh: '垂直凯尔特' },
  sin_z:        { en: 'sin(z)+c',                   zh: 'sin(z)+c' },
  cos_z:        { en: 'cos(z)+c',                   zh: 'cos(z)+c' },
  exp_z:        { en: 'exp(z)+c',                   zh: 'exp(z)+c' },
  sinh_z:       { en: 'sinh(z)+c',                  zh: 'sinh(z)+c' },
  cosh_z:       { en: 'cosh(z)+c',                  zh: 'cosh(z)+c' },
  tan_z:        { en: 'tan(z)+c',                   zh: 'tan(z)+c' },
}

export interface TransitionLegInput {
  variant: Variant | string
  weight: number
}

export type Metric = 'escape' | 'min_abs' | 'max_abs' | 'envelope' | 'min_pairwise_dist' | 'mandel_ship_agree'

export const METRICS: Metric[] = ['escape', 'min_abs', 'max_abs', 'envelope', 'min_pairwise_dist', 'mandel_ship_agree']

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
  | 'spectral1530'

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
]

export type LnMapColorMode = 'escape' | 'hist_eq' | 'row_eq' | 'log_lift' | 'bands' | 'frontier'
export type TransitionVideoMode = 'rotation' | 'zoom'

export interface MapRenderRequest {
  requestId?: string
  preemptKey?: string
  preemptSeq?: number
  taskType?: string
  localExport?: boolean
  background?: boolean
  centerRe: number
  centerIm: number
  centerReStr?: string
  centerImStr?: string
  scale: number          // height in complex units
  width: number
  height: number
  iterations: number
  variant?: Variant | string   // Variant literal or "custom:HASH"
  metric?: Metric
  colorMap?: ColorMap
  smooth?: boolean           // ln-smooth continuous coloring (μ = iter + 1 − log₂(log₂(|z|²)))
  colorMode?: 'direct' | 'eq_full' | 'eq_center'  // equalized preview (escape metric only)
  cyclesPerOctave?: number   // band density for equalized color modes
  bailout?: number
  bailoutSq?: number
  pairwiseCap?: number
  julia?: boolean
  juliaRe?: number
  juliaIm?: number
  transitionTheta?: number  // legacy radians/degrees; when set, transition kernel is used instead of the variant kernel
  transitionThetaMilliDeg?: number
  transitionFrom?: Variant | string
  transitionTo?: Variant | string
  transitionVariants?: Array<Variant | string>
  transitionWeights?: number[]
  transitionLegs?: TransitionLegInput[]
  engine?: string
  scalarType?: string
  rotationDeg?: number
}

export interface MapRenderResponse {
  runId: string
  requestId?: string
  status: string
  artifactId: string
  imagePath: string
  localPath?: string
  localExport?: boolean
  generatedMs: number
  width: number
  height: number
  effective: Record<string, any>
}

export interface MapRenderInlineResponse {
  status: string
  requestId?: string
  data?: ArrayBuffer
  generatedMs: number
  width: number
  height: number
  engineUsed?: string
  scalarUsed?: string
  pixelFormat?: string
}

export interface SpecialPoint {
  id: string
  family: string
  pointType: string
  k: number
  p: number
  real: number
  imag: number
  sourceMode: string
  createdAt: string
}

export type SpecialPointKind = 'center' | 'misiurewicz'

export interface SpecialPointViewport {
  centerRe: number
  centerIm: number
  // Full-precision decimal strings for deep zoom; override centerRe/centerIm
  // in the backend's high-precision solver when present.
  centerReStr?: string
  centerImStr?: string
  scale: number
  width: number
  height: number
}

export interface SpecialPointEnumRequest {
  kind: SpecialPointKind
  periodMin?: number
  periodMax?: number
  preperiodMin?: number
  preperiodMax?: number
  maxNewtonIter?: number
  maxSeedBatches?: number
  seedsPerBatch?: number
  includeVariantExistence?: boolean
  includeRejectedDebug?: boolean
  visibleOnly?: boolean
  viewport?: SpecialPointViewport
}

export interface SpecialPointSearchRequest {
  preemptKey?: string
  preemptSeq?: number
  kind?: SpecialPointKind
  periodMin?: number
  periodMax?: number
  preperiodMin?: number
  preperiodMax?: number
  seedBudget?: number
  maxNewtonIter?: number
  includeVariantCompatibility?: boolean
  visibleOnly?: boolean
  viewport: SpecialPointViewport
}

export interface SpecialPointSnapRequest {
  period: number
  re: number
  im: number
  maxNewtonIter?: number
  includeVariantCompatibility?: boolean
}

export interface OrbitClassification {
  kind?: string
  found_repeat: boolean
  is_center: boolean
  is_misiurewicz: boolean
  preperiod: number
  period: number
  repeat_error: number
}

export interface VariantExistence {
  variant_name: string
  exists: boolean
  same_orbit_as_mandelbrot: boolean
  actual_preperiod: number
  actual_period: number
  repeat_error: number
  reason: string
}

export interface SpecialPointEnumResult {
  id: string
  kind: SpecialPointKind
  preperiod: number
  period: number
  re: number
  im: number
  // Full-precision coordinates from the high-precision (deep zoom) solver.
  reStr?: string
  imStr?: string
  precBits?: number
  // Client-side only: offset from the current precise view center, computed
  // in MapView (BigDec) so deep-zoom markers project correctly.
  offsetRe?: number
  offsetIm?: number
  real?: number
  imag?: number
  converged: boolean
  success?: boolean
  accepted: boolean
  fallback?: boolean
  visible: boolean
  residual: number
  newtonIterations: number
  actual: OrbitClassification
  variants: VariantExistence[]
  compatibleVariants?: string[]
  variantCompatibility?: Record<string, any>
  reason: string
}

export interface SpecialPointSearchResponse {
  runId: string
  status: string
  sampled: boolean
  foundAny?: boolean
  noPoint?: boolean
  acceptedCount: number
  fallbackCount?: number
  seedCount: number
  newtonSuccessCount: number
  rejectedCount: number
  points: SpecialPointEnumResult[]
  warning?: string
  reportArtifactId?: string
  reportDownloadUrl?: string
}

export interface SpecialPointEnumResponse {
  runId: string
  complete: boolean
  status: string
  acceptedCount: number
  expectedCount: number
  seedCount: number
  newtonSuccessCount: number
  rejectedCount: number
  points: SpecialPointEnumResult[]
  rejected_debug?: SpecialPointEnumResult[]
  warning?: string
  reportArtifactId?: string
  reportDownloadUrl?: string
}

export interface MapFieldRequest {
  requestId?: string
  preemptKey?: string
  preemptSeq?: number
  centerRe: number
  centerIm: number
  centerReStr?: string
  centerImStr?: string
  scale: number
  width: number
  height: number
  iterations: number
  variant?: Variant | string   // Variant literal or "custom:HASH"
  metric?: Metric
  bailout?: number
  bailoutSq?: number
  pairwiseCap?: number
  julia?: boolean
  juliaRe?: number
  juliaIm?: number
  engine?: string
  scalarType?: string
  rotationDeg?: number
}

// Raw field response — no colorization applied.
// Escape metric:    iterB64 (uint32[W*H]) + finalMagB64 (float32[W*H] |z|²)
// Non-escape metric: fieldB64 (float64[W*H]) + fieldMin + fieldMax
export interface MapFieldResponse {
  status: string
  requestId?: string
  width: number
  height: number
  metric: string
  maxIter?: number
  iterB64?: string
  finalMagB64?: string
  fieldB64?: string
  fieldMin?: number
  fieldMax?: number
  generatedMs: number
  scalarUsed?: string
  engineUsed?: string
}

export interface LnMapRequest {
  centerRe: number
  centerIm: number
  centerReStr?: string
  centerImStr?: string
  julia?: boolean
  juliaRe?: number
  juliaIm?: number
  widthS?: number
  width?: number
  height?: number
  depthOctaves: number
  qualityPreset?: 'draft' | 'balanced' | 'high' | 'full' | 'custom'
  qualityScale?: number
  lnMapExtraOctaves?: number
  variant?: Variant
  colorMap?: ColorMap
  lnMapColorMode?: LnMapColorMode
  lnMapCyclesPerOctave?: number
  iterations?: number
  engine?: string
  precisionMode?: 'standard' | 'fast'
  scalarType?: string
  fastFp32DepthOctaves?: number
  fastFp64DepthOctaves?: number
  fastValidate?: boolean
  fastValidationBandOctaves?: number
  fastValidationSampleRows?: number
  fastValidationSampleCols?: number
  fastValidationMaxMismatchRatio?: number
  fastValidationMaxP99IterDelta?: number
  fastValidationMaxMeanColorDelta?: number
}

export interface LnMapResponse {
  runId: string
  status: string
  artifactId: string
  imagePath: string
  widthS: number
  heightT: number
  depthOctaves: number
  engineUsed?: string
  scalarUsed?: string
  precisionMode?: string
  lnMapColorMode?: LnMapColorMode
  lnMapCyclesPerOctave?: number
  layerSummary?: string
  validationSummary?: string
  generatedMs: number
}

export type HsStage = 'min_abs' | 'max_abs' | 'envelope' | 'min_pairwise_dist'

export interface HsMeshRequest {
  centerRe?: number
  centerIm?: number
  scale?: number
  width?: number
  height?: number
  resolution?: number
  metric?: HsStage
  variant?: Variant
  iterations?: number
  heightScale?: number
  pairwiseCap?: number
}

export interface HsFieldRequest {
  centerRe?: number
  centerIm?: number
  scale?: number
  resolution?: number
  metric?: HsStage
  variant?: Variant
  iterations?: number
  bailout?: number
  bailoutSq?: number
  heightClamp?: number
  pairwiseCap?: number
}

export interface HsFieldResponse {
  runId: string
  status: string
  width: number
  height: number
  fieldMin: number
  fieldMax: number
  fieldB64: string   // base64 float64[width * height], row-major
  generatedMs: number
}

export interface MeshResponse {
  runId: string
  status: string
  glbArtifactId: string
  glbUrl: string
  stlArtifactId: string
  stlUrl: string
  vertexCount: number
  triangleCount: number
  generatedMs?: number
  fieldMs?: number
  mcMs?: number
}

export interface TransitionMeshRequest {
  centerX?: number
  centerY?: number
  centerZ?: number
  extent?: number
  resolution?: number
  iso?: number
  iterations?: number
  bailout?: number
  bailoutSq?: number
  transitionFrom?: Variant | string
  transitionTo?: Variant | string
  transitionVariants?: Array<Variant | string>
  transitionWeights?: number[]
  transitionLegs?: TransitionLegInput[]
  engine?: string
}

export interface TransitionVoxelRequest {
  centerX?: number
  centerY?: number
  centerZ?: number
  extent?: number
  resolution?: number   // default 64, max 512
  iso?: number
  iterations?: number
  bailout?: number
  bailoutSq?: number
  transitionFrom?: Variant | string
  transitionTo?: Variant | string
  transitionVariants?: Array<Variant | string>
  transitionWeights?: number[]
  transitionLegs?: TransitionLegInput[]
  engine?: string
}

export interface TransitionVoxelResponse {
  runId: string
  status: string
  resolution: number
  extent: number
  voxelCount?: number
  faceCount: number
  generatedMs: number
  stlArtifactId?: string
  stlUrl?: string
  posB64: string    // float32[faceCount * 4 * 3] — world-space vertex positions
  normB64: string   // int8[faceCount * 3] — one outward normal per face
  depthB64: string  // uint8[faceCount] — depth byte (1=deep interior, 255=near surface)
}

// Unified export: ln-map + final frame + video in one request.
export interface VideoExportRequest {
  centerRe: number
  centerIm: number
  centerReStr?: string
  centerImStr?: string
  julia?: boolean
  juliaRe?: number
  juliaIm?: number
  variant?: Variant | string
  colorMap?: ColorMap
  iterations?: number
  bailout?: number
  bailoutSq?: number
  widthS?: number
  depthOctaves?: number
  fps?: number
  secondsPerOctave?: number
  durationSec?: number
  targetScale?: number
  qualityPreset?: 'draft' | 'balanced' | 'high' | 'full' | 'custom'
  qualityScale?: number
  lnMapEngine?: string
  lnMapMode?: 'standard' | 'fast'
  lnMapScalar?: string
  lnMapColorMode?: LnMapColorMode
  lnMapCyclesPerOctave?: number
  lnMapFastValidate?: boolean
  lnMapFastValidationBandOctaves?: number
  lnMapFastValidationSampleRows?: number
  lnMapFastValidationSampleCols?: number
  lnMapFastValidationMaxMismatchRatio?: number
  lnMapFastValidationMaxP99IterDelta?: number
  lnMapFastValidationMaxMeanColorDelta?: number
  lnMapExtraOctaves?: number
  lnMapMaxSegmentHeight?: number
  lnMapRunId?: string
  lnMapStatsRunId?: string
  lnMapPreviewRunId?: string
  cudaWarp?: boolean
  background?: boolean
  localExport?: boolean
  width?: number
  height?: number
  rotationDeg?: number
}

export interface VideoExportResponse {
  runId: string
  status: string
  videoArtifactId?: string
  videoUrl?: string
  videoDownloadUrl?: string
  videoLocalPath?: string
  lnMapArtifactId?: string
  lnMapDownloadUrl?: string
  lnMapLocalPath?: string
  finalFrameArtifactId?: string
  finalFrameDownloadUrl?: string
  finalFrameLocalPath?: string
  startFrameArtifactId?: string
  startFrameUrl?: string
  startFrameDownloadUrl?: string
  startFrameLocalPath?: string
  endFrameArtifactId?: string
  endFrameUrl?: string
  endFrameDownloadUrl?: string
  endFrameLocalPath?: string
  reportArtifactId?: string
  reportDownloadUrl?: string
  reportLocalPath?: string
  localExport?: boolean
  frameCount: number
  fps: number
  durationSec: number
  secondsPerOctave?: number
  depthOctaves?: number
  targetScale?: number
  rotationDeg?: number
  fullWidthS?: number
  actualWidthS?: number
  heightT?: number
  lnMapExtraOctaves?: number
  lnMapSegmented?: boolean
  lnMapSegmentCount?: number
  lnMapMaxSegmentHeight?: number
  lnMapTotalSegmentRows?: number
  qualityPreset?: string
  qualityScale?: number
  estimatedPeakMemory?: number
  estimatedSingleStripMemory?: number
  finalFrameEngine?: string
  finalFrameScalar?: string
  lnMapEngine?: string
  lnMapScalar?: string
  lnMapMode?: string
  lnMapColorMode?: LnMapColorMode
  lnMapStatsSource?: string
  lnMapStatsReused?: boolean
  lnMapCyclesPerOctave?: number
  lnMapLayerSummary?: string
  lnMapValidationSummary?: string
  warpMethod?: string
  encoder?: string
  ffmpegStderr?: string
  width: number
  height: number
  generatedMs?: number
}

export interface RunProgress {
  stage?: string
  current?: number
  total?: number
  percent?: number
  engine?: string
  scalar?: string
  elapsedMs?: number
  estimatedRemainingMs?: number | null
  cancelable?: boolean
  resourceLocks?: string[]
  depthOctave?: number
  totalDepthOctaves?: number
  currentFrame?: number
  totalFrames?: number
  currentLnMapRow?: number
  totalLnMapRows?: number
  currentLnMapSegment?: number
  lnMapSegmentCount?: number
  lnMapSegmentHeight?: number
  finalFrameEngine?: string
  finalFrameScalar?: string
  lnMapEngine?: string
  lnMapScalar?: string
  lnMapMode?: string
  lnMapColorMode?: LnMapColorMode
  lnMapPass?: 'equalization' | 'render' | string
  lnMapStatsSource?: string
  lnMapStatsReused?: boolean
  lnMapCyclesPerOctave?: number
  lnMapLayerSummary?: string
  lnMapValidationSummary?: string
  warpMethod?: string
  encoder?: string
  failedStage?: string
  errorMessage?: string
  details?: Record<string, any>
}

export interface RunArtifactStatus {
  artifactId: string
  name: string
  kind: string
  downloadUrl: string
  contentUrl: string
  localPath?: string
}

export interface RunStatusResponse {
  id: string
  module: string
  status: string
  startedAt: number
  finishedAt: number
  outputDir: string
  cancelRequested?: boolean
  progress: RunProgress
  artifacts: RunArtifactStatus[]
}

export interface ResourceLockStatus {
  name: string
  active: number
  limit: number
  busy: boolean
  activeRunId?: string
  taskType?: string
}

export interface ActiveTask {
  runId: string
  taskType: string
  status: string
  stage: string
  engine?: string
  scalar?: string
  startedAt: number
  elapsedMs: number
  cancelable: boolean
  cancelRequested: boolean
  progress: RunProgress
}

export interface ActiveTasksResponse {
  items: ActiveTask[]
  resourceLocks: ResourceLockStatus[]
}

export interface VideoPreviewRequest extends VideoExportRequest {
  previewWidth?: number
  previewHeight?: number
}

export interface VideoPreviewResponse {
  runId: string
  status: string
  startFrameArtifactId: string
  startFrameUrl: string
  startFrameDownloadUrl: string
  endFrameArtifactId: string
  endFrameUrl: string
  endFrameDownloadUrl: string
  frameCount: number
  fps: number
  durationSec: number
  secondsPerOctave: number
  depthOctaves: number
  targetScale: number
  rotationDeg?: number
  width: number
  height: number
  outputWidth: number
  outputHeight: number
  actualWidthS?: number
  heightT?: number
  lnMapArtifactId?: string
  lnMapDownloadUrl?: string
  finalFrameArtifactId?: string
  finalFrameDownloadUrl?: string
  lnMapColorMode?: LnMapColorMode
  lnMapStatsSource?: string
  lnMapCyclesPerOctave?: number
  lnMapEngine?: string
  lnMapScalar?: string
  lnMapMode?: string
  generatedMs: number
}

export interface TransitionVideoExportRequest {
  animationMode?: TransitionVideoMode
  centerRe: number
  centerIm: number
  centerReStr?: string
  centerImStr?: string
  julia?: boolean
  juliaRe?: number
  juliaIm?: number
  transitionFrom?: Variant | string
  transitionTo?: Variant | string
  colorMap?: ColorMap
  iterations?: number
  bailout?: number
  bailoutSq?: number
  scale?: number
  thetaStartDeg?: number
  thetaEndDeg?: number
  thetaDeg?: number
  depthOctaves?: number
  secondsPerOctave?: number
  targetScale?: number
  rotationDeg?: number
  durationSec?: number
  fps?: number
  metric?: string
  engine?: string
  scalarType?: string
  background?: boolean
  localExport?: boolean
  width?: number
  height?: number
}

export interface TransitionVideoExportResponse {
  runId: string
  status: string
  videoArtifactId?: string
  videoUrl?: string
  videoDownloadUrl?: string
  videoLocalPath?: string
  startFrameArtifactId?: string
  startFrameUrl?: string
  startFrameDownloadUrl?: string
  endFrameArtifactId?: string
  endFrameUrl?: string
  endFrameDownloadUrl?: string
  reportArtifactId?: string
  reportDownloadUrl?: string
  localExport?: boolean
  frameCount: number
  fps: number
  durationSec: number
  thetaStartDeg: number
  thetaEndDeg: number
  rotationDeg?: number
  transitionFrom: string
  transitionTo: string
  width: number
  height: number
  engine?: string
  scalarType?: string
  encoder?: string
  ffmpegStderr?: string
  generatedMs?: number
  animationMode?: TransitionVideoMode
  thetaDeg?: number
  depthOctaves?: number
  targetScale?: number
  secondsPerOctave?: number
}

export interface TransitionVideoPreviewRequest extends TransitionVideoExportRequest {
  previewWidth?: number
  previewHeight?: number
}

export interface TransitionVideoPreviewResponse {
  runId: string
  status: string
  startFrameArtifactId: string
  startFrameUrl: string
  startFrameDownloadUrl: string
  endFrameArtifactId: string
  endFrameUrl: string
  endFrameDownloadUrl: string
  thetaStartDeg: number
  thetaEndDeg: number
  rotationDeg?: number
  width: number
  height: number
  outputWidth: number
  outputHeight: number
  generatedMs: number
  animationMode?: TransitionVideoMode
  thetaDeg?: number
  frameCount?: number
  fps?: number
  durationSec?: number
  depthOctaves?: number
  targetScale?: number
  secondsPerOctave?: number
}

export interface VideoZoomRequest {
  lnMapArtifactId: string
  localExport?: boolean
  fps?: number
  durationSec?: number
  secondsPerOctave?: number
  targetScale?: number
  width?: number
  height?: number
  startLnRadius?: number
  depthOctaves?: number
  cudaWarp?: boolean
  rotationDeg?: number
}

export interface VideoZoomResponse {
  runId: string
  status: string
  artifactId: string
  videoUrl: string
  downloadUrl: string
  localPath?: string
  localExport?: boolean
  frameCount: number
  fps: number
  durationSec: number
  secondsPerOctave?: number
  depthOctaves?: number
  rotationDeg?: number
  width: number
  height: number
  warpMethod?: string
  encoder?: string
  ffmpegStderr?: string
  generatedMs: number
}

export interface Hardware {
  cpuModel: string
  cpuLogicalCores: number
  cpuPhysicalCores: number
  memoryTotalMiB: number
  memoryAvailableMiB: number
  gpuModel: string
  gpuMemory: string
}

export interface RunRow {
  id: string
  module: string
  status: string
  startedAt: number
  finishedAt: number
  outputDir: string
  cancelable?: boolean
  cancelRequested?: boolean
}

export interface ArtifactRow {
  artifactId: string
  runId: string
  name: string
  kind: string
  sizeBytes: number
  downloadPath: string
  contentPath: string
  localPath?: string
}

export interface CustomVariant {
  variantId:  string   // "custom:HASH"
  name:       string
  formula:    string
  bailout:    number
  bailoutSq?: number
  createdAt:  string
  loaded:     boolean
}

export interface BuiltinVariantInfo {
  variantId: string
  name:      string
  builtin:   true
}

export interface VariantListResponse {
  builtin: BuiltinVariantInfo[]
  custom:  CustomVariant[]
}

export interface VariantCompileResponse {
  ok:        boolean
  variantId?: string
  name?:     string
  hash?:     string
  bailout?:  number
  bailoutSq?: number
  cached?:   boolean
  error?:    string
}

// ---- API methods ----

export const api = {
  baseUrl: BASE,
  isLocalBrowserAccess,

  systemCheck: () => getJson<{ openmp: boolean; cuda: boolean }>('/api/system/check'),
  hardware:    () => getJson<Hardware>('/api/system/hardware'),
  capabilities:() => getJson<Record<string, any>>('/api/system/capabilities'),

  mapRender:  (req: MapRenderRequest, signal?: AbortSignal)  => postJson<MapRenderResponse>('/api/map/render', req, signal),
  mapRenderInline: async (req: MapRenderRequest, signal?: AbortSignal): Promise<MapRenderInlineResponse> => {
    const res = await postArrayBuffer('/api/map/render-inline', req, signal)
    const status = res.headers.get('X-FSD-Status') ?? (res.status === 204 ? 'cancelled' : 'completed')
    return {
      status,
      requestId: res.headers.get('X-FSD-Request-Id') ?? undefined,
      data: res.data,
      generatedMs: numberHeader(res.headers, 'X-FSD-Generated-Ms', 0),
      width: numberHeader(res.headers, 'X-FSD-Width', req.width),
      height: numberHeader(res.headers, 'X-FSD-Height', req.height),
      engineUsed: res.headers.get('X-FSD-Engine') ?? undefined,
      scalarUsed: res.headers.get('X-FSD-Scalar') ?? undefined,
      pixelFormat: res.headers.get('X-FSD-Pixel-Format') ?? undefined,
    }
  },
  mapPreempt: (req: Pick<MapRenderRequest, 'preemptKey' | 'preemptSeq'>) =>
    postJson<{ status: string; preemptKey?: string; preemptSeq?: number }>('/api/map/preempt', req),
  mapField:   (req: MapFieldRequest, signal?: AbortSignal)   => postJson<MapFieldResponse>('/api/map/field', req, signal),
  lnMap:      (req: LnMapRequest)      => postJson<LnMapResponse>('/api/map/ln', req),

  specialPointsAuto: (k: number, p: number, pointType?: string) =>
    postJson<{ mode: string; k: number; p: number; count: number; points: SpecialPoint[] }>(
      '/api/special-points/auto', { k, p, pointType }),

  specialPointsSeed: (k: number, p: number, re: number, im: number) =>
    postJson<{ mode: string; converged: boolean; points: SpecialPoint[] }>(
      '/api/special-points/seed', { k, p, re, im }),

  specialPointsList: (family?: string) =>
    getJson<{ items: SpecialPoint[] }>(
      `/api/special-points${family ? `?family=${encodeURIComponent(family)}` : ''}`),

  specialPointsEnumerate: (req: SpecialPointEnumRequest) =>
    postJson<SpecialPointEnumResponse>('/api/special-points/enumerate', req),

  specialPointsSearch: (req: SpecialPointSearchRequest, signal?: AbortSignal) =>
    postJson<SpecialPointSearchResponse>('/api/special-points/search', req, signal),

  specialPointsResults: (runId: string) =>
    getJson<SpecialPointSearchResponse>(`/api/special-points/results?runId=${encodeURIComponent(runId)}`),

  specialPointsSnap: (req: SpecialPointSnapRequest) =>
    postJson<{ point: SpecialPointEnumResult }>('/api/special-points/snap', req),

  hsMesh:  (req: HsMeshRequest)  => postJson<MeshResponse>('/api/hs/mesh', req),
  hsField: (req: HsFieldRequest) => postJson<HsFieldResponse>('/api/hs/field', req),
  transitionMesh:   (req: TransitionMeshRequest)  => postJson<MeshResponse>('/api/transition/mesh', req),
  transitionVoxels: (req: TransitionVoxelRequest) => postJson<TransitionVoxelResponse>('/api/transition/voxels', req),
  videoZoom:   (req: VideoZoomRequest)   => postJson<VideoZoomResponse>('/api/video/zoom', req),
  videoPreview:(req: VideoPreviewRequest)=> postJson<VideoPreviewResponse>('/api/video/preview', req),
  videoExport: (req: VideoExportRequest) => postJson<VideoExportResponse>('/api/video/export', req),
  transitionVideoPreview: (req: TransitionVideoPreviewRequest) => postJson<TransitionVideoPreviewResponse>('/api/video/transition-preview', req),
  transitionVideoExport:  (req: TransitionVideoExportRequest)  => postJson<TransitionVideoExportResponse>('/api/video/transition', req),

  runs: (params?: { limit?: number; offset?: number; module?: string; status?: string }) => {
    const q = new URLSearchParams()
    if (params?.limit  !== undefined) q.set('limit',  String(params.limit))
    if (params?.offset !== undefined) q.set('offset', String(params.offset))
    if (params?.module) q.set('module', params.module)
    if (params?.status) q.set('status', params.status)
    const s = q.toString()
    return getJson<{ items: RunRow[]; totalCount: number; modules: string[] }>(`/api/runs${s ? '?' + s : ''}`)
  },
  runStatus: (runId: string) =>
    getJson<RunStatusResponse>(`/api/runs/status?runId=${encodeURIComponent(runId)}`),
  activeTasks: () => getJson<ActiveTasksResponse>('/api/tasks/active'),
  cancelRun: (runId: string) =>
    postJson<{ runId: string; status: string; accepted: boolean; cancelRequested: boolean }>(`/api/runs/${encodeURIComponent(runId)}/cancel`, {}),
  benchmark: (req: Record<string, any> = {}) =>
    postJson<Record<string, any>>('/api/benchmark', req),

  artifacts: (kind?: string, runId?: string) => {
    const q = new URLSearchParams()
    if (kind)  q.set('kind',  kind)
    if (runId) q.set('runId', runId)
    const s = q.toString()
    return getJson<{ items: ArtifactRow[] }>(`/api/artifacts${s ? '?' + s : ''}`)
  },

  artifactContentUrl: (artifactId: string) =>
    `${BASE}/api/artifacts/content?artifactId=${encodeURIComponent(artifactId)}`,

  artifactDownloadUrl: (artifactId: string) =>
    `${BASE}/api/artifacts/download?artifactId=${encodeURIComponent(artifactId)}`,

  variantList:    () =>
    getJson<VariantListResponse>('/api/variants'),
  variantCompile: (formula: string, name: string, bailout?: number) =>
    postJson<VariantCompileResponse>(
      '/api/variants/compile',
      bailout === undefined ? { formula, name } : { formula, name, bailout },
    ),
  variantDelete:  (variantId: string) =>
    postJson<{ ok: boolean }>('/api/variants/delete', { variantId }),
}
