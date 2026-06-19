// api.ts — typed client for the native fractal_studio backend.
//
// All endpoints are POST JSON / GET query-string. Returns parsed JSON.

const BASE =
  (import.meta as any).env?.VITE_BACKEND_URL ??
  `http://${location.hostname}:18080`

export function isLocalBrowserAccess(): boolean {
  const host = location.hostname.toLowerCase()
  return host === 'localhost' || host.startsWith('127.') || host === '::1'
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
  mandelbrot:   { en: 'Mandelbrot',                 zh: 'Mandelbrot' },
  tricorn:      { en: 'Tricorn / Mandelbar',        zh: 'Tricorn / Mandelbar' },
  burning_ship: { en: 'Burning Ship',               zh: 'Burning Ship' },
  celtic:       { en: 'Perpendicular Burning Ship', zh: 'Perpendicular Burning Ship' },
  heart:        { en: 'Perpendicular Mandelbrot',   zh: 'Perpendicular Mandelbrot' },
  buffalo:      { en: 'Celtic',                     zh: 'Celtic' },
  perp_buffalo: { en: 'Mandelbar Celtic',           zh: 'Mandelbar Celtic' },
  celtic_ship:  { en: 'Buffalo',                    zh: 'Buffalo' },
  mandelceltic: { en: 'Perpendicular Buffalo',      zh: 'Perpendicular Buffalo' },
  perp_ship:    { en: 'Perpendicular Celtic',       zh: 'Perpendicular Celtic' },
  sin_z:        { en: 'sin(z)+c',                   zh: 'sin(z)+c' },
  cos_z:        { en: 'cos(z)+c',                   zh: 'cos(z)+c' },
  exp_z:        { en: 'exp(z)+c',                   zh: 'exp(z)+c' },
  sinh_z:       { en: 'sinh(z)+c',                  zh: 'sinh(z)+c' },
  cosh_z:       { en: 'cosh(z)+c',                  zh: 'cosh(z)+c' },
  tan_z:        { en: 'tan(z)+c',                   zh: 'tan(z)+c' },
}

export type Metric = 'escape' | 'min_abs' | 'max_abs' | 'envelope' | 'min_pairwise_dist'

export const METRICS: Metric[] = ['escape', 'min_abs', 'max_abs', 'envelope', 'min_pairwise_dist']

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

export interface MapRenderRequest {
  requestId?: string
  preemptKey?: string
  preemptSeq?: number
  taskType?: string
  localExport?: boolean
  centerRe: number
  centerIm: number
  scale: number          // height in complex units
  width: number
  height: number
  iterations: number
  variant?: Variant | string   // Variant literal or "custom:HASH"
  metric?: Metric
  colorMap?: ColorMap
  smooth?: boolean           // ln-smooth continuous coloring (μ = iter + 1 − log₂(log₂(|z|²)))
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
  engine?: string
  scalarType?: string
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
  centerRe: number
  centerIm: number
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
}

// Raw field response — no colorization applied.
// Escape metric:    iterB64 (uint32[W*H]) + finalMagB64 (float32[W*H] |z|²)
// Non-escape metric: fieldB64 (float64[W*H]) + fieldMin + fieldMax
export interface MapFieldResponse {
  status: string
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
}

export interface LnMapRequest {
  centerRe: number
  centerIm: number
  julia?: boolean
  juliaRe?: number
  juliaIm?: number
  widthS?: number
  width?: number
  height?: number
  depthOctaves: number
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
  cudaWarp?: boolean
  background?: boolean
  localExport?: boolean
  width?: number
  height?: number
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
  fullWidthS?: number
  actualWidthS?: number
  heightT?: number
  lnMapExtraOctaves?: number
  qualityPreset?: string
  qualityScale?: number
  estimatedPeakMemory?: number
  finalFrameEngine?: string
  finalFrameScalar?: string
  lnMapEngine?: string
  lnMapScalar?: string
  lnMapMode?: string
  lnMapColorMode?: LnMapColorMode
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
  finalFrameEngine?: string
  finalFrameScalar?: string
  lnMapEngine?: string
  lnMapScalar?: string
  lnMapMode?: string
  lnMapColorMode?: LnMapColorMode
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
  width: number
  height: number
  outputWidth: number
  outputHeight: number
  generatedMs: number
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
  mapField:   (req: MapFieldRequest)   => postJson<MapFieldResponse>('/api/map/field', req),
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

  runs: (limit = 50) => getJson<{ items: RunRow[] }>(`/api/runs?limit=${limit}`),
  runStatus: (runId: string) =>
    getJson<RunStatusResponse>(`/api/runs/status?runId=${encodeURIComponent(runId)}`),
  activeTasks: () => getJson<ActiveTasksResponse>('/api/tasks/active'),
  cancelRun: (runId: string) =>
    postJson<{ runId: string; status: string }>(`/api/runs/${encodeURIComponent(runId)}/cancel`, {}),
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
