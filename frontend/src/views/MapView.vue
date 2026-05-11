<script setup lang="ts">
import { inject, onMounted, ref, watch, computed } from 'vue'
import MapCanvas from '../components/MapCanvas.vue'
import SpecialPointList from '../components/SpecialPointList.vue'
import {
  api, VARIANTS, METRICS, COLORMAPS, VARIANT_LABELS,
  type Metric, type ColorMap, type SpecialPoint,
  type SpecialPointEnumResult,
  type VideoExportResponse, type VideoPreviewResponse, type RunProgress, type RunStatusResponse, type CustomVariant,
} from '../api'
import type { StatusState } from '../types'
import { t, lang } from '../i18n'
import { isMobileDevice } from '../device'

// Metric display labels
const METRIC_LABELS: Record<string, { en: string; zh: string }> = {
  escape:             { en: 'Escape time',   zh: '逃逸时间' },
  min_abs:            { en: 'Min |z|',       zh: '最小 |z|' },
  max_abs:            { en: 'Max |z|',       zh: '最大 |z|' },
  envelope:           { en: 'Envelope',      zh: '包络' },
  min_pairwise_dist:  { en: 'Min pairwise',  zh: '最小轨道距' },
}

const COLORMAP_LABELS: Record<string, { en: string; zh: string }> = {
  classic_cos: { en: 'Classic Cos', zh: '经典余弦' },
  mod17:       { en: 'Mod-17',      zh: 'Mod-17' },
  hsv_wheel:   { en: 'HSV Wheel',   zh: 'HSV 色轮' },
  tri765:      { en: 'Tri-765',     zh: 'Tri-765' },
  grayscale:   { en: 'Grayscale',   zh: '灰度' },
  hs_rainbow:  { en: 'HS Rainbow',  zh: '隐结构彩虹' },
}

const status = inject<StatusState>('status')!
const localExportMode = computed(() => api.isLocalBrowserAccess())
const pngExportStatus = ref('')

type ExportPreset = {
  key: string
  label: { en: string; zh: string }
  width: number
  height: number
}

const EXPORT_PRESETS: ExportPreset[] = [
  { key: 'fhd',       label: { en: 'FHD 16:9',       zh: 'FHD 16:9' },       width: 1920, height: 1080 },
  { key: 'qhd',       label: { en: 'QHD 16:9',       zh: 'QHD 16:9' },       width: 2560, height: 1440 },
  { key: '4k',        label: { en: '4K UHD 16:9',    zh: '4K UHD 16:9' },    width: 3840, height: 2160 },
  { key: '8k',        label: { en: '8K UHD 16:9',    zh: '8K UHD 16:9' },    width: 7680, height: 4320 },
  { key: 'wuxga',     label: { en: 'WUXGA 16:10',    zh: 'WUXGA 16:10' },    width: 1920, height: 1200 },
  { key: 'wqxga',     label: { en: 'WQXGA 16:10',    zh: 'WQXGA 16:10' },    width: 2560, height: 1600 },
  { key: 'uwqhd',     label: { en: 'UWQHD 21:9',     zh: 'UWQHD 21:9' },     width: 3440, height: 1440 },
  { key: 'phone_fhd', label: { en: 'Phone 9:16 FHD', zh: '手机 9:16 FHD' },  width: 1080, height: 1920 },
  { key: 'phone_qhd', label: { en: 'Phone 9:16 QHD', zh: '手机 9:16 QHD' },  width: 1440, height: 2560 },
]

// ── Left / Mandelbrot viewport ────────────────────────────────────────────────
const centerRe   = ref(-0.75)
const centerIm   = ref( 0.0)
const scale      = ref( 3.0)
const iterations = ref(1024)

const variant  = ref<string>('mandelbrot')  // Variant literal or "custom:HASH"
const metric   = ref<Metric>('escape')
const colorMap = ref<ColorMap>('classic_cos')
const smooth   = ref(false)

// ── Custom variants ───────────────────────────────────────────────────────────
const customVariants     = ref<CustomVariant[]>([])
const showCustomPanel    = ref(false)
const customFormula      = ref('z^2 + c')
const customName         = ref('my_variant')
const customBailoutDirty = ref(false)
const customBailout      = ref(suggestCustomBailout(customFormula.value))
const customCompiling    = ref(false)
const customCompileMsg   = ref('')

function suggestCustomBailout(formula: string): number {
  const s = formula.replace(/\s+/g, '').toLowerCase()
  const power = s === 'z*z+c'
    ? 2
    : s === 'z*z*z+c'
      ? 3
      : Number(s.match(/^z\^(\d+)\+c$/)?.[1] ?? s.match(/^pow\(z,(\d+)\)\+c$/)?.[1])
  return Number.isFinite(power) && power >= 2 ? Math.pow(2, 1 / (power - 1)) : 2
}

watch(customFormula, (formula) => {
  if (!customBailoutDirty.value) customBailout.value = suggestCustomBailout(formula)
})

async function loadCustomVariants() {
  try {
    const r = await api.variantList()
    customVariants.value = r.custom
  } catch {}
}

async function compileCustom() {
  customCompiling.value = true
  customCompileMsg.value = ''
  try {
    const r = await api.variantCompile(
      customFormula.value,
      customName.value,
      customBailoutDirty.value ? customBailout.value : undefined,
    )
    if (r.ok && r.variantId) {
      await loadCustomVariants()
      variant.value        = r.variantId
      showCustomPanel.value = false
      customCompileMsg.value = ''
    } else {
      customCompileMsg.value = r.error ?? 'compile failed'
    }
  } catch (e: any) {
    customCompileMsg.value = e?.message ?? 'error'
  } finally {
    customCompiling.value = false
  }
}

async function deleteCustom(variantId: string) {
  await api.variantDelete(variantId)
  await loadCustomVariants()
  if (variant.value === variantId) variant.value = 'mandelbrot'
}

function onVariantSelect(val: string) {
  if (val === '__new_custom__') {
    showCustomPanel.value = true
    // keep previous variant active until compile succeeds
  } else {
    variant.value = val
    showCustomPanel.value = false
  }
}

const transitionOn = ref(false)
const pointsCollapsed = ref(isMobileDevice)
const specialPointResults = ref<SpecialPointEnumResult[]>([])
const hoveredSpecialPointId = ref('')
const selectedSpecialPointId = ref('')
const specialPointVariantHint = ref('')
const mapViewportW = ref(1200)
const mapViewportH = ref(800)
const transitionFrom = ref<string>('mandelbrot')
const transitionTo   = ref<string>('burning_ship')
const AXIS_TRANSITION_VARIANTS = VARIANTS.slice(0, 10)
const THETA_SCALE = 1000
const THETA_HALF_TURN = 180 * THETA_SCALE
const THETA_FULL_TURN = 360 * THETA_SCALE
const thetaMilliDeg = ref(45 * THETA_SCALE)

function normalizeThetaMilliDeg(mdeg: number): number {
  if (!Number.isFinite(mdeg)) return 0
  const raw = Math.round(mdeg)
  let wrapped = ((raw + THETA_HALF_TURN) % THETA_FULL_TURN + THETA_FULL_TURN) % THETA_FULL_TURN - THETA_HALF_TURN
  if (wrapped === -THETA_HALF_TURN && raw > 0) wrapped = THETA_HALF_TURN
  return wrapped
}

function setThetaDeg(deg: number) {
  thetaMilliDeg.value = normalizeThetaMilliDeg(deg * THETA_SCALE)
}

function nudgeThetaDeg(delta: number) {
  thetaMilliDeg.value = normalizeThetaMilliDeg(thetaMilliDeg.value + delta * THETA_SCALE)
}

watch(thetaMilliDeg, (mdeg) => {
  const normalized = normalizeThetaMilliDeg(mdeg)
  if (normalized !== mdeg) thetaMilliDeg.value = normalized
})

const thetaDeg = computed({
  get: () => thetaMilliDeg.value / THETA_SCALE,
  set: (deg: number) => setThetaDeg(deg),
})
const transitionThetaMilliDeg = computed(() => normalizeThetaMilliDeg(thetaMilliDeg.value))
const activeTransitionThetaMilliDeg = computed(() => transitionOn.value ? transitionThetaMilliDeg.value : null)

const SPECIAL_POINT_VARIANT_BY_ID: Record<string, string> = {
  mandelbrot: 'Mandelbrot',
  tricorn: 'Tri',
  burning_ship: 'Boat',
  celtic: 'Duck',
  heart: 'Bell',
  buffalo: 'Fish',
  perp_buffalo: 'Vase',
  celtic_ship: 'Bird',
  mandelceltic: 'Mask',
  perp_ship: 'Ship',
}

const SPECIAL_POINT_VARIANT_ID_BY_NAME: Record<string, string> = Object.fromEntries(
  Object.entries(SPECIAL_POINT_VARIANT_BY_ID).map(([id, name]) => [name, id])
)

function specialPointBackendVariantName(id = variant.value): string {
  return SPECIAL_POINT_VARIANT_BY_ID[id] ?? ''
}

function specialPointVariantLabel(backendName: string): string {
  const id = SPECIAL_POINT_VARIANT_ID_BY_NAME[backendName]
  return id ? ((VARIANT_LABELS as any)[id]?.[lang.value] ?? backendName) : backendName
}

function specialPointVariantNames(p: SpecialPointEnumResult): string[] {
  const names = new Set<string>(['Mandelbrot'])
  for (const v of p.compatibleVariants || []) names.add(v)
  for (const v of p.variants || []) if (v.exists) names.add(v.variant_name)
  return [...names]
}

function specialPointExistsInVariant(p: SpecialPointEnumResult, backendName: string): boolean {
  if (backendName === 'Mandelbrot') return true
  if (!backendName) return false
  if (p.compatibleVariants?.includes(backendName)) return true
  return !!p.variants?.some(v => v.variant_name === backendName && v.exists)
}

function specialPointMatchesCurrentVariant(p: SpecialPointEnumResult): boolean {
  const backendName = specialPointBackendVariantName()
  if (backendName === 'Mandelbrot') return true
  return specialPointExistsInVariant(p, backendName)
}

function updateSpecialPointVariantHint(p: SpecialPointEnumResult | null) {
  if (!p) {
    specialPointVariantHint.value = ''
    return
  }
  const currentBackend = specialPointBackendVariantName()
  const compatible = specialPointVariantNames(p)
    .filter(name => name !== 'Mandelbrot')
    .map(specialPointVariantLabel)
  if (currentBackend === 'Mandelbrot' && compatible.length) {
    specialPointVariantHint.value = `Also visible in variants: ${compatible.join(', ')}`
  } else if (currentBackend && currentBackend !== 'Mandelbrot' && specialPointExistsInVariant(p, currentBackend)) {
    specialPointVariantHint.value = `Retained Mandelbrot special point for ${specialPointVariantLabel(currentBackend)}`
  } else {
    specialPointVariantHint.value = ''
  }
}

function mergeSpecialPointCache(points: SpecialPointEnumResult[]) {
  if (!points.length) return
  const byId = new Map<string, SpecialPointEnumResult>()
  for (const p of specialPointResults.value) byId.set(p.id, p)
  for (const p of points) {
    const existing = byId.get(p.id)
    if (!existing || p.residual < existing.residual) byId.set(p.id, p)
  }
  specialPointResults.value = [...byId.values()].sort((a, b) =>
    a.period - b.period || a.preperiod - b.preperiod || a.re - b.re || a.im - b.im
  )
}

// ── Julia mode ────────────────────────────────────────────────────────────────
const juliaOn  = ref(false)
const juliaRe  = ref(-0.7)
const juliaIm  = ref(0.27)

// Right / Julia viewport (independent)
const jCenterRe = ref(0.0)
const jCenterIm = ref(0.0)
const jScale    = ref(4.0)

// Format c for display
const juliaLabel = computed(() => {
  const sign = juliaIm.value >= 0 ? '+' : ''
  return `${juliaRe.value.toPrecision(10)} ${sign}${juliaIm.value.toPrecision(10)}i`
})

// Left-canvas click: pick julia c AND recenter left map
function onPickJulia(pos: { re: number; im: number }) {
  juliaRe.value = pos.re
  juliaIm.value = pos.im
  centerRe.value = pos.re
  centerIm.value = pos.im
}

function onJuliaViewport(v: { centerRe: number; centerIm: number; scale: number }) {
  jCenterRe.value = v.centerRe
  jCenterIm.value = v.centerIm
  jScale.value    = v.scale
}

// ── Engine / scalar ───────────────────────────────────────────────────────────
const engineMode = ref<'auto' | 'openmp' | 'avx2' | 'avx512' | 'cuda' | 'hybrid'>('auto')
const scalarMode = ref<'auto' | 'fp32' | 'fp64' | 'fx64'>('auto')

// ── Status rail sync ─────────────────────────────────────────────────────────
const lastMs         = ref<number | null>(null)
const lastArtifactId = ref<string>('')
const lastEngine     = ref('')
const lastScalar     = ref('')

function syncStatus() {
  status.cRe      = centerRe.value
  status.cIm      = centerIm.value
  status.zoom     = scale.value
  status.iter     = iterations.value
  status.variant  = variant.value
  status.metric   = metric.value
  status.renderMs = lastMs.value
  status.engine   = lastEngine.value || engineMode.value
  status.scalar   = lastScalar.value || scalarMode.value
  status.message  = 'ready'
}

watch([centerRe, centerIm, scale, iterations, variant, metric, lastMs], syncStatus, { immediate: true })

onMounted(() => {
  loadCustomVariants()

  const pending = sessionStorage.getItem('fs_pending_center')
  if (pending) {
    try {
      const c = JSON.parse(pending)
      if (typeof c.re === 'number' && typeof c.im === 'number') {
        centerRe.value = c.re
        centerIm.value = c.im
        if (typeof c.scale === 'number' && Number.isFinite(c.scale) && c.scale > 0) {
          scale.value = c.scale
        }
      }
    } catch {}
    sessionStorage.removeItem('fs_pending_center')
  }
})

function onViewportChange(v: { centerRe: number; centerIm: number; scale: number }) {
  centerRe.value = v.centerRe
  centerIm.value = v.centerIm
  scale.value    = v.scale
}

function onMapViewportSize(size: { width: number; height: number }) {
  if (size.width >= 16) mapViewportW.value = size.width
  if (size.height >= 16) mapViewportH.value = size.height
}

function onRendered(meta: { generatedMs: number; artifactId?: string; engineUsed?: string; scalarUsed?: string }) {
  lastMs.value         = meta.generatedMs
  lastArtifactId.value = meta.artifactId ?? ''
  lastEngine.value     = meta.engineUsed ?? ''
  lastScalar.value     = meta.scalarUsed ?? ''
  syncStatus()
}

function resetView() {
  if (juliaOn.value) {
    jCenterRe.value = 0.0
    jCenterIm.value = 0.0
    jScale.value    = 4.0
    return
  }
  centerRe.value = 0.0
  centerIm.value = 0.0
  scale.value    = 4.0
}

function onImportPoint(p: SpecialPoint | SpecialPointEnumResult) {
  centerRe.value = 're' in p ? p.re : p.real
  centerIm.value = 'im' in p ? p.im : p.imag
}

const specialPointViewport = computed(() => ({
  centerRe: centerRe.value,
  centerIm: centerIm.value,
  scale: scale.value,
  width: mapViewportW.value,
  height: mapViewportH.value,
}))

function pointInCurrentView(p: SpecialPointEnumResult) {
  const aspect = specialPointViewport.value.width / specialPointViewport.value.height
  const halfH = scale.value * 0.5
  const halfW = halfH * aspect
  return p.re >= centerRe.value - halfW && p.re <= centerRe.value + halfW
    && p.im >= centerIm.value - halfH && p.im <= centerIm.value + halfH
}

const visibleSpecialPoints = computed(() =>
  specialPointResults.value.filter(p => pointInCurrentView(p) && specialPointMatchesCurrentVariant(p))
)
const renderedSpecialPoints = computed(() =>
  pointsCollapsed.value ? [] : visibleSpecialPoints.value
)

function onSpecialPointResults(points: SpecialPointEnumResult[]) {
  mergeSpecialPointCache(points)
  hoveredSpecialPointId.value = ''
  const selected = specialPointResults.value.find(p => p.id === selectedSpecialPointId.value) ?? null
  if (selectedSpecialPointId.value && !selected) {
    selectedSpecialPointId.value = ''
    updateSpecialPointVariantHint(null)
  } else {
    updateSpecialPointVariantHint(selected)
  }
}

function onSpecialPointHover(id: string) {
  hoveredSpecialPointId.value = id
}

function onSpecialPointSelect(p: SpecialPointEnumResult) {
  selectedSpecialPointId.value = p.id
  updateSpecialPointVariantHint(p)
  onImportPoint(p)
}

function onUseSpecialPointAsJulia(p: SpecialPointEnumResult) {
  juliaOn.value = true
  juliaRe.value = p.re
  juliaIm.value = p.im
}

watch(variant, () => {
  const selected = specialPointResults.value.find(p => p.id === selectedSpecialPointId.value) ?? null
  updateSpecialPointVariantHint(selected)
})

const pngPresetKey = ref('fhd')
const videoPresetKey = ref('fhd')

const pngPreset = computed(() =>
  EXPORT_PRESETS.find(p => p.key === pngPresetKey.value) ?? EXPORT_PRESETS[0]
)
const videoPreset = computed(() =>
  EXPORT_PRESETS.find(p => p.key === videoPresetKey.value) ?? EXPORT_PRESETS[0]
)

async function exportPng() {
  pngExportStatus.value = ''
  try {
    const resp = await api.mapRender({
      taskType:   'still_export',
      localExport: localExportMode.value,
      centerRe:   centerRe.value,
      centerIm:   centerIm.value,
      scale:      scale.value,
      width:      pngPreset.value.width,
      height:     pngPreset.value.height,
      iterations: iterations.value,
      variant:    variant.value,
      metric:     metric.value,
      colorMap:   colorMap.value,
      smooth:     smooth.value,
      engine:     engineMode.value,
      scalarType: scalarMode.value,
      julia:      juliaOn.value,
      juliaRe:    juliaRe.value,
      juliaIm:    juliaIm.value,
      transitionTheta: transitionOn.value ? transitionThetaMilliDeg.value * Math.PI / (180 * THETA_SCALE) : undefined,
      transitionThetaMilliDeg: transitionOn.value ? transitionThetaMilliDeg.value : undefined,
      transitionFrom:  transitionOn.value ? transitionFrom.value : undefined,
      transitionTo:    transitionOn.value ? transitionTo.value : undefined,
    }) as any
    if (localExportMode.value && resp.localPath) {
      pngExportStatus.value = `${lang.value === 'en' ? 'saved locally' : '已保存到本地'} · ${resp.localPath}`
      status.message = pngExportStatus.value
      return
    }
    window.open(api.artifactDownloadUrl(resp.artifactId), '_blank')
  } catch (e: any) {
    pngExportStatus.value = 'failed: ' + (e?.data?.error || e?.message || e)
    console.error('export PNG failed:', e?.data?.error ?? e)
  }
}

// ── Unified video export (ln-map + final frame + video in one dialog) ─────────
const exportModalOpen = ref(false)
const exportDepth     = ref(20)
const exportFps       = ref(30)
const exportSecondsPerOctave = ref(0.4)
const exportQualityPreset = ref<'draft' | 'balanced' | 'high' | 'full'>('balanced')
const exportLnMapMode = ref<'standard' | 'fast'>('fast')
const exportLnMapScalar = ref<'auto' | 'fp64' | 'fx64'>('auto')
const exportW         = ref(1920)
const exportH         = ref(1080)
const exportBusy      = ref(false)
const exportPreviewBusy = ref(false)
const exportStatus    = ref('')
const exportPreviewStatus = ref('')
const exportResult    = ref<VideoExportResponse | null>(null)
const exportPreviewResult = ref<VideoPreviewResponse | null>(null)
const exportJobId     = ref('')
const exportProgress  = ref<RunProgress>({})
const exportDepthDirty = ref(false)

const exportEstimatedDuration = computed(() =>
  Math.max(0, exportDepth.value) * Math.max(0, exportSecondsPerOctave.value)
)
const exportEstimatedFrames = computed(() =>
  Math.max(2, Math.round(exportEstimatedDuration.value * Math.max(1, exportFps.value)))
)
const visiblePreview = computed(() => exportPreviewResult.value ?? exportResult.value)

function fmtDurationMs(ms?: number | null): string {
  if (typeof ms !== 'number' || !Number.isFinite(ms) || ms < 0) return ''
  if (ms < 1000) return '<1s'
  const totalSec = Math.ceil(ms / 1000)
  const h = Math.floor(totalSec / 3600)
  const m = Math.floor((totalSec % 3600) / 60)
  const s = totalSec % 60
  if (h > 0) return `${h}h ${m}m`
  if (m > 0) return `${m}m ${s}s`
  return `${s}s`
}

function etaSuffix(ms?: number | null): string {
  const eta = fmtDurationMs(ms)
  return eta ? ` · ETA ${eta}` : ''
}

const exportProgressDetail = computed(() => {
  const p = exportProgress.value
  if (!p.stage) return ''
  if (p.stage === 'ln_map') {
    return `ln-map ${p.current || 0}/${p.total || 0} rows · octave ${(p.depthOctave || 0).toFixed(2)}/${(p.totalDepthOctaves || 0).toFixed(2)}${etaSuffix(p.estimatedRemainingMs)}`
  }
  if (p.stage === 'video_warp_encode') {
    return `encode ${p.current || 0}/${p.total || 0} frames${etaSuffix(p.estimatedRemainingMs)}`
  }
  if (p.stage === 'final_frame') return `final frame ${p.current || 0}/${p.total || 1}${etaSuffix(p.estimatedRemainingMs)}`
  return p.stage
})
const exportMemoryEstimateMiB = computed(() => {
  const fullWidth = Math.ceil(Math.sqrt(exportW.value * exportW.value + exportH.value * exportH.value) * Math.PI)
  const scaleByPreset: Record<string, number> = { draft: 0.35, balanced: 0.55, high: 0.75, full: 1.0 }
  const actualWidth = Math.ceil(fullWidth * (scaleByPreset[exportQualityPreset.value] ?? 0.55))
  const heightT = Math.ceil((2 + exportDepth.value) * Math.LN2 / (Math.PI * 2) * actualWidth)
  const pixels = exportW.value * exportH.value
  const bytes = actualWidth * heightT * 3 + pixels * (3 * 4 + 4 * 5 + 1)
  return bytes / 1024 / 1024
})

watch(videoPreset, p => {
  exportW.value = p.width
  exportH.value = p.height
  if (exportModalOpen.value && !exportDepthDirty.value) syncExportDepthToCurrentView()
}, { immediate: true })

watch([exportW, exportH], () => {
  if (exportModalOpen.value && !exportDepthDirty.value) syncExportDepthToCurrentView()
  if (exportModalOpen.value) clearExportPreview()
})

function defaultExportDepthForView() {
  const aspect = Math.max(1e-9, exportW.value / Math.max(1, exportH.value))
  const rMax = Math.sqrt(aspect * aspect + 1)
  const kTopStart = Math.log(4) - Math.log(rMax)
  const kTopEnd = Math.log(Math.max(scale.value, 1e-300) * 0.5)
  const depth = (kTopStart - kTopEnd) / Math.LN2
  if (!Number.isFinite(depth)) return 20
  return Math.min(120, Math.max(0.05, depth))
}

function syncExportDepthToCurrentView() {
  exportDepth.value = Number(defaultExportDepthForView().toFixed(2))
}

function clearExportPreview() {
  exportPreviewStatus.value = ''
  exportPreviewResult.value = null
}

function progressRatio(stage: string) {
  if (exportProgress.value.stage !== stage) {
    if (stage === 'final_frame' && ['ln_map', 'video_warp_encode'].includes(exportProgress.value.stage || '')) return 1
    if (stage === 'ln_map' && exportProgress.value.stage === 'video_warp_encode') return 1
    return 0
  }
  const total = Math.max(1, exportProgress.value.total || 1)
  return Math.max(0, Math.min(1, (exportProgress.value.current || 0) / total))
}

function onExportDepthInput() {
  exportDepthDirty.value = true
  clearExportPreview()
}

function openExportModal() {
  exportDepthDirty.value = false
  syncExportDepthToCurrentView()
  exportModalOpen.value = true
  exportStatus.value    = ''
  exportPreviewStatus.value = ''
  exportResult.value    = null
  exportPreviewResult.value = null
  exportJobId.value = ''
  exportProgress.value = {}
}

function videoRequestBase() {
  return {
    centerRe:     centerRe.value,
    centerIm:     centerIm.value,
    julia:        juliaOn.value,
    juliaRe:      juliaRe.value,
    juliaIm:      juliaIm.value,
    variant:      variant.value,
    colorMap:     colorMap.value,
    iterations:   Math.max(iterations.value, 2048),
    depthOctaves: exportDepth.value,
    fps:          exportFps.value,
    secondsPerOctave: exportSecondsPerOctave.value,
    targetScale:  !exportDepthDirty.value && exportDepth.value > 0.05 ? scale.value : undefined,
    qualityPreset: exportQualityPreset.value,
    lnMapMode:    exportLnMapMode.value,
    lnMapScalar:  exportLnMapScalar.value,
    background: true,
    localExport:  localExportMode.value,
    width:        exportW.value,
    height:       exportH.value,
  }
}

function previewSizeForExport() {
  const maxSide = 720
  const longSide = Math.max(exportW.value, exportH.value, 1)
  const ratio = Math.min(1, maxSide / longSide)
  return {
    previewWidth: Math.max(64, Math.round(exportW.value * ratio)),
    previewHeight: Math.max(64, Math.round(exportH.value * ratio)),
  }
}

async function runPreview() {
  exportPreviewBusy.value = true
  exportPreviewStatus.value = 'previewing…'
  exportPreviewResult.value = null
  try {
    const resp = await api.videoPreview({
      ...videoRequestBase(),
      ...previewSizeForExport(),
    })
    exportPreviewResult.value = resp
    exportPreviewStatus.value = `${resp.width}×${resp.height} · depth ${resp.depthOctaves.toFixed(2)} · ${resp.generatedMs.toFixed(0)} ms`
  } catch (e: any) {
    exportPreviewStatus.value = 'failed: ' + (e?.data?.error || e?.message || e)
  } finally {
    exportPreviewBusy.value = false
  }
}

async function runExport() {
  exportBusy.value   = true
  exportStatus.value = 'queued…'
  exportResult.value = null
  exportProgress.value = {}
  try {
    const resp = await api.videoExport(videoRequestBase())
    exportJobId.value = resp.runId
    exportStatus.value = `${resp.runId} · ${resp.frameCount} frames · ${resp.durationSec.toFixed(2)}s`
    await pollVideoExport(resp)
  } catch (e: any) {
    exportStatus.value = 'failed: ' + (e?.data?.error || e?.message || e)
  } finally {
    exportBusy.value = false
  }
}

function artifactByName(status: RunStatusResponse, name: string) {
  return status.artifacts.find(a => a.name === name)
}

async function pollVideoExport(initial: VideoExportResponse) {
  for (;;) {
    await new Promise(resolve => setTimeout(resolve, 700))
    const status = await api.runStatus(initial.runId)
    exportProgress.value = status.progress || {}
    if (status.status === 'failed') {
      const msg = status.progress?.errorMessage || 'video export failed'
      exportStatus.value = `failed: ${status.progress?.failedStage || status.progress?.stage || 'video_export'} · ${msg}`
      return
    }
    if (status.status === 'cancelled') {
      exportStatus.value = 'cancelled'
      return
    }
    if (status.status !== 'completed') continue

    const video = artifactByName(status, 'zoom.mp4') || status.artifacts.find(a => a.kind === 'video')
    const lnMap = artifactByName(status, 'ln_map.png')
    const finalFrame = artifactByName(status, 'final_frame.png')
    const startFrame = artifactByName(status, 'start_frame.png')
    const endFrame = artifactByName(status, 'end_frame.png')
    const localExport = initial.localExport ?? localExportMode.value
    exportResult.value = {
      ...initial,
      status: 'completed',
      localExport,
      videoArtifactId: video?.artifactId,
      videoUrl: video?.contentUrl,
      videoDownloadUrl: video?.downloadUrl,
      videoLocalPath: video?.localPath,
      lnMapArtifactId: lnMap?.artifactId,
      lnMapDownloadUrl: lnMap?.downloadUrl,
      lnMapLocalPath: lnMap?.localPath,
      finalFrameArtifactId: finalFrame?.artifactId,
      finalFrameDownloadUrl: finalFrame?.downloadUrl,
      finalFrameLocalPath: finalFrame?.localPath,
      startFrameArtifactId: startFrame?.artifactId,
      startFrameUrl: startFrame?.contentUrl,
      startFrameDownloadUrl: startFrame?.downloadUrl,
      startFrameLocalPath: startFrame?.localPath,
      endFrameArtifactId: endFrame?.artifactId,
      endFrameUrl: endFrame?.contentUrl,
      endFrameDownloadUrl: endFrame?.downloadUrl,
      endFrameLocalPath: endFrame?.localPath,
      generatedMs: status.finishedAt && status.startedAt ? status.finishedAt - status.startedAt : undefined,
    }
    exportStatus.value = localExport && video?.localPath
      ? `completed · ${initial.frameCount} frames · ${video.localPath}`
      : `completed · ${initial.frameCount} frames`
    return
  }
}
</script>

<template>
  <div class="map-view">

    <!-- ── Controls bar ──────────────────────────────────────────────────── -->
    <div class="controls">
      <div class="group">
        <label>{{ t('variant') }}</label>
        <select :value="variant" @change="onVariantSelect(($event.target as HTMLSelectElement).value)" :disabled="transitionOn">
          <option v-for="v in VARIANTS" :key="v" :value="v">{{ VARIANT_LABELS[v][lang] }}</option>
          <template v-if="customVariants.length">
            <option disabled>──────</option>
            <option v-for="cv in customVariants" :key="cv.variantId" :value="cv.variantId">
              ✦ {{ cv.name }}
            </option>
          </template>
          <option value="__new_custom__">{{ t('custom_new') }}</option>
        </select>
      </div>

      <div class="group">
        <label>{{ t('metric') }}</label>
        <select v-model="metric">
          <option v-for="m in METRICS" :key="m" :value="m">{{ METRIC_LABELS[m]?.[lang] ?? m }}</option>
        </select>
      </div>

      <div class="group">
        <label>{{ t('colormap') }}</label>
        <select v-model="colorMap">
          <option v-for="c in COLORMAPS" :key="c" :value="c">{{ COLORMAP_LABELS[c]?.[lang] ?? c }}</option>
        </select>
      </div>

      <div class="group">
        <label>
          <input type="checkbox" v-model="smooth" style="width:auto;margin-right:6px" />
          {{ t('smooth') }}
        </label>
      </div>

      <div class="group">
        <label>{{ t('iterations') }}</label>
        <input type="number" v-model.number="iterations" min="16" max="1000000" step="128" />
      </div>

      <div class="group transition-group">
        <label>
          <input type="checkbox" v-model="transitionOn" style="width:auto;margin-right:6px" />
          {{ t('transition') }}
        </label>
        <div v-if="transitionOn" class="theta-row">
          <button class="theta-loop-btn" @click="nudgeThetaDeg(-15)">−</button>
          <input type="range" min="-180" max="180" step="0.1" v-model.number="thetaDeg" />
          <button class="theta-loop-btn" @click="nudgeThetaDeg(15)">+</button>
          <input class="theta-input num" type="number" min="-180" max="180" step="0.1" :value="thetaDeg.toFixed(1)" @change="setThetaDeg(Number(($event.target as HTMLInputElement).value))" />
          <span class="num">°</span>
        </div>
        <div v-if="transitionOn" class="theta-row theta-snaps">
          <button @click="setThetaDeg(-180)">−180</button>
          <button @click="setThetaDeg(-90)">−90</button>
          <button @click="setThetaDeg(0)">0</button>
          <button @click="setThetaDeg(90)">90</button>
          <button @click="setThetaDeg(180)">180</button>
        </div>
        <div v-if="transitionOn" class="theta-row">
          <select v-model="transitionFrom">
            <option v-for="v in AXIS_TRANSITION_VARIANTS" :key="'from-' + v" :value="v">{{ VARIANT_LABELS[v][lang] }}</option>
          </select>
          <span class="num">→</span>
          <select v-model="transitionTo">
            <option v-for="v in AXIS_TRANSITION_VARIANTS" :key="'to-' + v" :value="v">{{ VARIANT_LABELS[v][lang] }}</option>
          </select>
        </div>
      </div>

      <div class="group">
        <label>
          <input type="checkbox" v-model="juliaOn" style="width:auto;margin-right:6px" />
          {{ t('julia') }}
        </label>
      </div>

      <div class="group">
        <label>{{ t('engine') }}</label>
        <select v-model="engineMode">
          <option value="auto">auto</option>
          <option value="cuda">cuda</option>
          <option value="avx2">avx2</option>
          <option value="avx512">avx512</option>
          <option value="hybrid">hybrid</option>
          <option value="openmp">openmp</option>
        </select>
      </div>

      <div class="group">
        <label>{{ t('scalar') }}</label>
        <select v-model="scalarMode">
          <option value="auto">auto</option>
          <option value="fp32">fp32</option>
          <option value="fp64">fp64</option>
          <option value="fx64">fx64</option>
        </select>
      </div>

      <div class="spacer"></div>

      <div class="group export-preset-group">
        <label>{{ lang === 'en' ? 'Wallpaper' : '壁纸尺寸' }}</label>
        <select v-model="pngPresetKey">
          <option v-for="p in EXPORT_PRESETS" :key="p.key" :value="p.key">
            {{ p.label[lang] }} · {{ p.width }}×{{ p.height }}
          </option>
        </select>
      </div>

      <button @click="resetView" :title="juliaOn ? t('reset_julia') : t('reset')">
        ⌂ {{ juliaOn ? t('reset_julia') : t('reset') }}
      </button>
      <button @click="exportPng">{{ t('export_png') }}</button>
      <button @click="openExportModal">{{ t('export_video') }}</button>
      <span v-if="pngExportStatus" class="export-local-status mono">{{ pngExportStatus }}</span>
    </div>

    <!-- ── Custom formula editor ─────────────────────────────────────────── -->
    <div v-if="showCustomPanel" class="custom-panel">
      <div class="custom-header mono">
        <span>{{ t('custom_new') }}</span>
        <button class="custom-close" @click="showCustomPanel = false">✕</button>
      </div>
      <div class="custom-hint mono">{{ t('custom_hint') }}</div>
      <div class="custom-row">
        <label>{{ t('custom_formula') }}</label>
        <input v-model="customFormula" class="formula-input mono" placeholder="z^2 + c" spellcheck="false" />
        <label style="margin-left:12px">{{ t('custom_name') }}</label>
        <input v-model="customName" placeholder="my_variant" style="width:120px" />
        <label style="margin-left:12px">{{ t('custom_bailout') }}</label>
        <input type="number" v-model.number="customBailout" min="0.1" max="1000000" step="0.001" style="width:80px" @input="customBailoutDirty = true" />
        <button class="btn-compile" @click="compileCustom" :disabled="customCompiling">
          {{ customCompiling ? t('loading') : t('custom_compile') }}
        </button>
      </div>
      <div v-if="customCompileMsg" class="custom-msg mono">{{ customCompileMsg }}</div>
      <div v-if="customVariants.length" class="custom-list">
        <div v-for="cv in customVariants" :key="cv.variantId" class="custom-item mono">
          <span class="cv-name">{{ cv.name }}</span>
          <span class="cv-formula">{{ cv.formula }}</span>
          <button class="cv-use" @click="variant = cv.variantId; showCustomPanel = false">use</button>
          <button class="cv-del" @click="deleteCustom(cv.variantId)">{{ t('custom_delete') }}</button>
        </div>
      </div>
    </div>

    <!-- ── Julia info strip ─────────────────────────────────────────────── -->
    <div v-if="juliaOn" class="julia-strip mono">
      <span class="julia-label">{{ t('julia_selected_c') }}:</span>
      <span class="julia-val">{{ juliaLabel }}</span>
      <span class="julia-hint">{{ t('julia_hint') }}</span>
    </div>

    <!-- ── Main stage: dual-pane or single ──────────────────────────────── -->
    <div class="stage" :class="{ 'points-collapsed': pointsCollapsed }">

      <!-- Single-pane mode (no Julia) -->
      <template v-if="!juliaOn">
        <MapCanvas
          :centerRe="centerRe" :centerIm="centerIm" :scale="scale"
          :iterations="iterations" :variant="variant" :metric="metric"
          :colorMap="colorMap" :smooth="smooth"
          :transitionTheta="transitionOn ? transitionThetaMilliDeg * Math.PI / (180 * THETA_SCALE) : null"
          :transition-theta-milli-deg="activeTransitionThetaMilliDeg"
          :transitionFrom="transitionFrom" :transitionTo="transitionTo"
          :engine="engineMode" :scalarType="scalarMode"
          :special-points="renderedSpecialPoints"
          :hovered-special-point-id="hoveredSpecialPointId"
          :selected-special-point-id="selectedSpecialPointId"
          @viewport-change="onViewportChange"
          @viewport-size="onMapViewportSize"
          @rendered="onRendered"
          @hover-special-point="onSpecialPointHover"
          @select-special-point="onSpecialPointSelect"
        />
        <aside :class="['points', { collapsed: pointsCollapsed }]">
          <button
            class="points-toggle"
            :title="pointsCollapsed ? '展开根列表' : '折叠根列表'"
            @click="pointsCollapsed = !pointsCollapsed">
            <span>{{ pointsCollapsed ? '‹' : '›' }}</span>
          </button>
          <SpecialPointList
            v-if="!pointsCollapsed"
            :viewport="specialPointViewport"
            :hovered-id="hoveredSpecialPointId"
            :selected-id="selectedSpecialPointId"
            :variant-hint="specialPointVariantHint"
            @import-point="onImportPoint"
            @hover-point="onSpecialPointHover"
            @select-point="onSpecialPointSelect"
            @results-updated="onSpecialPointResults"
            @use-julia="onUseSpecialPointAsJulia"
          />
        </aside>
      </template>

      <!-- Dual-pane Julia mode -->
      <template v-else>
        <div class="dual-pane">
          <!-- Left: Mandelbrot / variant — click picks julia c -->
          <div class="pane">
            <div class="pane-header mono">
              <span class="pane-title">{{ t('julia_left') }}: {{ (VARIANT_LABELS as any)[variant]?.[lang] ?? variant }}</span>
              <span class="pane-meta">
                {{ t('center') }}: {{ centerRe.toPrecision(10) }} + {{ centerIm.toPrecision(10) }}i
                &nbsp;·&nbsp;{{ t('scale') }}: {{ scale.toPrecision(6) }}
              </span>
            </div>
            <div class="pane-canvas">
              <MapCanvas
                :centerRe="centerRe" :centerIm="centerIm" :scale="scale"
                :iterations="iterations" :variant="variant" :metric="metric"
                :colorMap="colorMap" :smooth="smooth"
                :transitionTheta="transitionOn ? transitionThetaMilliDeg * Math.PI / (180 * THETA_SCALE) : null"
                :transition-theta-milli-deg="activeTransitionThetaMilliDeg"
                :transitionFrom="transitionFrom" :transitionTo="transitionTo"
                :engine="engineMode" :scalarType="scalarMode"
                :special-points="renderedSpecialPoints"
                :hovered-special-point-id="hoveredSpecialPointId"
                :selected-special-point-id="selectedSpecialPointId"
                @viewport-change="onViewportChange"
                @viewport-size="onMapViewportSize"
                @rendered="onRendered"
                @click-world="onPickJulia"
                @hover-special-point="onSpecialPointHover"
                @select-special-point="onSpecialPointSelect"
              />
            </div>
          </div>

          <!-- Right: Julia set J(c) — own viewport -->
          <div class="pane">
            <div class="pane-header mono">
              <span class="pane-title">{{ t('julia_right') }}</span>
              <span class="pane-meta">
                julia c: {{ juliaRe.toPrecision(10) }} + {{ juliaIm.toPrecision(10) }}i
                &nbsp;·&nbsp;{{ t('center') }}: {{ jCenterRe.toPrecision(6) }} + {{ jCenterIm.toPrecision(6) }}i
                &nbsp;·&nbsp;{{ t('scale') }}: {{ jScale.toPrecision(6) }}
              </span>
            </div>
            <div class="pane-canvas">
              <MapCanvas
                :centerRe="jCenterRe" :centerIm="jCenterIm" :scale="jScale"
                :iterations="iterations" :variant="variant" :metric="metric"
                :colorMap="colorMap" :smooth="smooth"
                :transition-theta="null"
                :julia="true" :juliaRe="juliaRe" :juliaIm="juliaIm"
                :engine="engineMode" :scalarType="scalarMode"
                @viewport-change="onJuliaViewport"
              />
            </div>
          </div>
        </div>
      </template>
    </div>

    <!-- ── Unified video export modal ───────────────────────────────────── -->
    <Teleport to="body">
      <div v-if="exportModalOpen" class="modal-backdrop" @click.self="exportModalOpen = false">
        <div class="modal">
          <div class="modal-title">
            {{ juliaOn ? t('export_julia_video') : t('export_video') }}
          </div>
          <div v-if="juliaOn" class="mrow source mono" style="margin-bottom:6px">
            Julia c: {{ juliaRe.toPrecision(8) }} + {{ juliaIm.toPrecision(8) }}i
          </div>
          <div class="modal-body">
            <div class="mrow">
              <label>{{ t('video_depth') }}</label>
              <input type="number" v-model.number="exportDepth" min="0.05" max="120" step="0.05" @input="onExportDepthInput" />
            </div>
            <div class="mrow">
              <label>{{ t('video_fps') }}</label>
              <input type="number" v-model.number="exportFps" min="1" max="120" step="1" />
            </div>
            <div class="mrow">
              <label>{{ t('video_seconds_per_octave') }}</label>
              <input type="number" v-model.number="exportSecondsPerOctave" min="0.05" max="60" step="0.05" />
            </div>
            <div class="mrow estimate">
              <label>{{ t('video_estimate') }}</label>
              <span class="mono">{{ exportEstimatedDuration.toFixed(2) }}s · {{ exportEstimatedFrames }} frames · {{ exportMemoryEstimateMiB.toFixed(0) }} MiB</span>
            </div>
            <div v-if="localExportMode" class="mrow local-mode-row">
              <label>{{ lang === 'en' ? 'Output' : '输出' }}</label>
              <span class="mono">{{ lang === 'en' ? 'local mode: save to backend runtime/runs, no browser download by default' : '本地模式：默认写入后端 runtime/runs，不走浏览器下载' }}</span>
            </div>
            <div class="mrow">
              <label>Quality</label>
              <select v-model="exportQualityPreset">
                <option value="draft">draft</option>
                <option value="balanced">balanced</option>
                <option value="high">high</option>
                <option value="full">full</option>
              </select>
            </div>
            <div class="mrow">
              <label>ln-map mode</label>
              <select v-model="exportLnMapMode">
                <option value="fast">fast · depth layered</option>
                <option value="standard">standard · full precision</option>
              </select>
            </div>
            <div class="mrow">
              <label>ln-map scalar</label>
              <select v-model="exportLnMapScalar">
                <option value="auto">auto</option>
                <option value="fp64">fp64</option>
                <option value="fx64">fx64</option>
              </select>
            </div>
            <div class="mrow">
              <label>{{ lang === 'en' ? 'Preset' : '预设' }}</label>
              <select v-model="videoPresetKey">
                <option v-for="p in EXPORT_PRESETS" :key="p.key" :value="p.key">
                  {{ p.label[lang] }} · {{ p.width }}×{{ p.height }}
                </option>
              </select>
            </div>
            <div class="mrow">
              <label>{{ t('video_width') }}</label>
              <input type="number" v-model.number="exportW" min="128" max="7680" step="64" />
            </div>
            <div class="mrow">
              <label>{{ t('video_height') }}</label>
              <input type="number" v-model.number="exportH" min="128" max="4320" step="64" />
            </div>
          </div>
          <div class="modal-footer">
            <button @click="exportModalOpen = false" class="btn-cancel">{{ t('video_cancel') }}</button>
            <button @click="runPreview" :disabled="exportBusy || exportPreviewBusy" class="btn-preview">
              {{ exportPreviewBusy ? t('loading') : t('video_preview') }}
            </button>
            <button @click="runExport" :disabled="exportBusy || exportPreviewBusy" class="btn-go">
              {{ exportBusy ? t('loading') : t('video_render') }}
            </button>
          </div>
          <div v-if="exportPreviewStatus" class="modal-status mono">{{ exportPreviewStatus }}</div>
          <div v-if="exportStatus" class="modal-status mono">{{ exportStatus }}</div>
          <div v-if="exportBusy || exportJobId" class="progress-stack">
            <div class="progress-row">
              <span>final</span>
              <progress :value="progressRatio('final_frame')" max="1"></progress>
            </div>
            <div class="progress-row">
              <span>ln-map</span>
              <progress :value="progressRatio('ln_map')" max="1"></progress>
            </div>
            <div class="progress-row">
              <span>encode</span>
              <progress :value="progressRatio('video_warp_encode')" max="1"></progress>
            </div>
          </div>
          <div v-if="exportProgressDetail" class="modal-status mono">{{ exportProgressDetail }}</div>
          <div v-if="visiblePreview" class="modal-body" style="gap:6px">
            <div v-if="visiblePreview.startFrameUrl || visiblePreview.endFrameUrl" class="preview-grid">
              <a v-if="visiblePreview.startFrameUrl"
                 :href="api.baseUrl + (visiblePreview.startFrameDownloadUrl || visiblePreview.startFrameUrl)"
                 class="preview-item" download>
                <span>{{ t('video_start_frame') }}</span>
                <img :src="api.baseUrl + visiblePreview.startFrameUrl" alt="" />
              </a>
              <a v-if="visiblePreview.endFrameUrl"
                 :href="api.baseUrl + (visiblePreview.endFrameDownloadUrl || visiblePreview.endFrameUrl)"
                 class="preview-item" download>
                <span>{{ t('video_end_frame') }}</span>
                <img :src="api.baseUrl + visiblePreview.endFrameUrl" alt="" />
              </a>
            </div>
          </div>
          <div v-if="exportResult" class="modal-body" style="gap:6px">
            <div v-if="exportResult.localExport" class="local-path-list mono">
              <div class="local-path-title">{{ lang === 'en' ? 'Saved locally' : '已保存到本地' }}</div>
              <div v-if="exportResult.videoLocalPath">video: {{ exportResult.videoLocalPath }}</div>
              <div v-if="exportResult.lnMapLocalPath">ln-map: {{ exportResult.lnMapLocalPath }}</div>
              <div v-if="exportResult.finalFrameLocalPath">final: {{ exportResult.finalFrameLocalPath }}</div>
            </div>
            <template v-else>
              <a v-if="exportResult.videoDownloadUrl" :href="api.baseUrl + exportResult.videoDownloadUrl" class="dl-link" download>↓ {{ t('video_download') }}</a>
              <a v-if="exportResult.lnMapDownloadUrl" :href="api.baseUrl + exportResult.lnMapDownloadUrl" class="dl-link" download>↓ ln-map PNG</a>
              <a v-if="exportResult.finalFrameDownloadUrl" :href="api.baseUrl + exportResult.finalFrameDownloadUrl" class="dl-link" download>↓ {{ t('export_png') }}</a>
            </template>
          </div>
        </div>
      </div>
    </Teleport>

  </div>
</template>

<style scoped>
.map-view {
  display: flex;
  flex-direction: column;
  height: 100%;
  overflow: hidden;
}

/* ── Controls ── */
.controls {
  display: flex;
  align-items: flex-end;
  gap: 14px;
  padding: 10px 14px;
  border-bottom: 1px solid var(--rule);
  background: var(--panel);
  flex-wrap: wrap;
  flex-shrink: 0;
}

.group {
  display: flex;
  flex-direction: column;
  min-width: 100px;
}

.group.transition-group { min-width: 290px; }
.group.export-preset-group { min-width: 190px; }
.export-local-status {
  max-width: min(620px, 100%);
  color: var(--text-dim);
  font-size: 10px;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.theta-row {
  display: flex;
  align-items: center;
  gap: 8px;
}
.theta-row input[type="range"] { flex: 1; }

.theta-loop-btn {
  width: 26px;
  min-width: 26px;
  padding: 3px 0;
}

.theta-input {
  width: 68px;
  min-width: 68px;
}

.theta-snaps {
  gap: 4px;
}

.theta-snaps button {
  flex: 1;
  min-width: 42px;
  padding: 3px 4px;
  font-size: 10px;
}

.spacer { flex: 1; }

/* ── Julia strip ── */
.julia-strip {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 5px 14px;
  background: var(--accent-weak);
  border-bottom: 1px solid var(--accent-edge);
  font-size: var(--fs-label);
  flex-shrink: 0;
}
.julia-label { color: var(--text-dim); text-transform: uppercase; letter-spacing: 0.08em; }
.julia-val   { color: var(--accent); }
.julia-hint  { color: var(--text-faint); margin-left: auto; }

/* ── Stage ── */
.stage {
  flex: 1;
  min-height: 0;
  display: grid;
  grid-template-columns: 1fr 320px;
}

.stage.points-collapsed {
  grid-template-columns: 1fr 28px;
}

/* single-pane: canvas takes full grid, points panel on right */
.stage > .map-canvas-wrap,
.stage > canvas { grid-column: 1; }

.points {
  position: relative;
  border-left: 1px solid var(--rule);
  padding: 12px 14px 12px 18px;
  background: var(--bg-raised);
  overflow-y: auto;
  min-width: 0;
}

.points.collapsed {
  padding: 8px 0;
  overflow: hidden;
}

.points-toggle {
  position: absolute;
  top: 8px;
  left: 3px;
  width: 20px;
  min-width: 20px;
  height: 24px;
  padding: 0;
  z-index: 2;
  border-color: transparent;
  color: var(--text-faint);
}

.points-toggle:hover {
  border-color: var(--rule-hi);
  color: var(--accent);
}

.points-toggle span {
  display: block;
  font-size: 16px;
  line-height: 1;
  letter-spacing: 0;
}

.points.collapsed .points-toggle {
  position: static;
  margin: 0 auto;
}

/* ── Dual pane ── */
.dual-pane {
  grid-column: 1 / -1;
  display: grid;
  grid-template-columns: 1fr 1fr;
  height: 100%;
  min-height: 0;
}

.pane {
  display: flex;
  flex-direction: column;
  min-height: 0;
  border-right: 1px solid var(--rule);
}
.pane:last-child { border-right: none; }

.pane-header {
  display: flex;
  flex-direction: column;
  gap: 2px;
  padding: 6px 10px;
  background: var(--panel);
  border-bottom: 1px solid var(--rule);
  flex-shrink: 0;
}
.pane-title {
  font-size: var(--fs-label);
  text-transform: uppercase;
  letter-spacing: 0.08em;
  color: var(--accent);
}
.pane-meta {
  font-size: 10px;
  color: var(--text-dim);
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}

.pane-canvas {
  flex: 1;
  min-height: 0;
  position: relative;
}

/* ── Video modal ── */
.modal-backdrop {
  position: fixed;
  inset: 0;
  background: rgba(0,0,0,0.7);
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: 9999;
}
.modal {
  background: var(--panel);
  border: 1px solid var(--rule);
  width: min(420px, calc(100vw - 32px));
  padding: 24px 28px;
  display: flex;
  flex-direction: column;
  gap: 14px;
}
.modal-title {
  font-family: var(--mono);
  font-size: 12px;
  color: var(--accent);
  text-transform: uppercase;
  letter-spacing: 0.1em;
}
.modal-body { display: flex; flex-direction: column; gap: 10px; }
.mrow {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 10px;
}
.mrow label {
  font-size: 11px;
  color: var(--text-dim);
  text-transform: uppercase;
  letter-spacing: 0.05em;
  min-width: 90px;
}
.mrow input[type="number"] { width: 100px; }
.mrow.estimate span {
  font-size: 10px;
  color: var(--text-dim);
  white-space: nowrap;
}
.mrow.source {
  flex-direction: column;
  align-items: flex-start;
  font-size: 10px;
  color: var(--text-dim);
  line-height: 1.5;
}
.modal-footer { display: flex; gap: 10px; justify-content: flex-end; }
.btn-cancel {
  background: transparent;
  border: 1px solid var(--rule);
  color: var(--text-dim);
  padding: 6px 14px;
  font-family: var(--mono);
  font-size: 12px;
  cursor: pointer;
}
.btn-preview {
  background: transparent;
  border: 1px solid var(--accent);
  color: var(--accent);
  padding: 6px 14px;
  font-family: var(--mono);
  font-size: 12px;
  cursor: pointer;
}
.btn-go {
  background: var(--accent);
  color: #000;
  border: none;
  padding: 6px 16px;
  font-family: var(--mono);
  font-size: 12px;
  font-weight: 600;
  cursor: pointer;
}
.btn-go:disabled,
.btn-preview:disabled { opacity: 0.5; cursor: default; }
.modal-status { font-size: 10px; color: var(--text-dim); }
.local-mode-row span {
  color: var(--text-dim);
  font-size: 10px;
}
.progress-stack {
  display: flex;
  flex-direction: column;
  gap: 6px;
}
.progress-row {
  display: grid;
  grid-template-columns: 58px 1fr;
  align-items: center;
  gap: 8px;
  font-family: var(--mono);
  font-size: 10px;
  color: var(--text-dim);
}
.progress-row progress {
  width: 100%;
  height: 7px;
  accent-color: var(--accent);
}
.preview-grid {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 8px;
  margin-bottom: 4px;
}
.preview-item {
  display: flex;
  flex-direction: column;
  gap: 5px;
  color: var(--text-dim);
  font-family: var(--mono);
  font-size: 10px;
  text-decoration: none;
}
.preview-item img {
  width: 100%;
  height: 120px;
  object-fit: contain;
  border: 1px solid var(--rule);
  background: #000;
}
.preview-item:hover span { color: var(--accent); }
.dl-link {
  display: block;
  padding: 5px 0;
  font-family: var(--mono);
  font-size: 11px;
  color: var(--accent);
  text-decoration: none;
}
.dl-link:hover { text-decoration: underline; }
.local-path-list {
  display: flex;
  flex-direction: column;
  gap: 4px;
  padding: 8px;
  border: 1px solid var(--rule);
  color: var(--text-dim);
  font-size: 10px;
  overflow-wrap: anywhere;
}
.local-path-title {
  color: var(--text);
  font-weight: 700;
}

/* ── Custom formula panel ── */
.custom-panel {
  background: var(--panel);
  border-bottom: 1px solid var(--rule);
  padding: 10px 14px;
  display: flex;
  flex-direction: column;
  gap: 8px;
  flex-shrink: 0;
}
.custom-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  font-size: 11px;
  color: var(--accent);
  text-transform: uppercase;
  letter-spacing: 0.08em;
}
.custom-close {
  background: none;
  border: none;
  color: var(--text-dim);
  cursor: pointer;
  font-size: 14px;
  padding: 0 4px;
}
.custom-close:hover { color: var(--text); }
.custom-hint {
  font-size: 10px;
  color: var(--text-dim);
  line-height: 1.5;
}
.custom-row {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-wrap: wrap;
}
.custom-row label {
  font-size: 11px;
  color: var(--text-dim);
  text-transform: uppercase;
  letter-spacing: 0.05em;
  white-space: nowrap;
}
.formula-input {
  flex: 1;
  min-width: 180px;
}
.btn-compile {
  background: var(--accent);
  color: #000;
  border: none;
  padding: 5px 14px;
  font-family: var(--mono);
  font-size: 12px;
  font-weight: 600;
  cursor: pointer;
}
.btn-compile:disabled { opacity: 0.5; cursor: default; }
.custom-msg {
  font-size: 10px;
  color: var(--bad);
  white-space: pre-wrap;
}
.custom-list {
  display: flex;
  flex-direction: column;
  gap: 4px;
  border-top: 1px solid var(--rule);
  padding-top: 6px;
}
.custom-item {
  display: flex;
  align-items: center;
  gap: 10px;
  font-size: 11px;
}
.cv-name { color: var(--accent); min-width: 100px; }
.cv-formula { color: var(--text-dim); flex: 1; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.cv-use, .cv-del {
  background: none;
  border: 1px solid var(--rule);
  color: var(--text-dim);
  font-family: var(--mono);
  font-size: 10px;
  padding: 2px 8px;
  cursor: pointer;
}
.cv-use:hover { color: var(--accent); border-color: var(--accent); }
.cv-del:hover { color: var(--bad);    border-color: var(--bad); }





































@media (max-width: 760px), ((pointer: coarse) and (max-width: 1200px)), ((any-pointer: coarse) and (max-width: 1200px)) {
  .map-view {
    height: 100%;
    min-height: 0;
  }

  .controls {
    flex-wrap: nowrap;
    align-items: stretch;
    gap: 8px;
    padding: 8px;
    overflow-x: auto;
    overflow-y: hidden;
    scrollbar-width: none;
    -webkit-overflow-scrolling: touch;
  }

  .controls::-webkit-scrollbar {
    display: none;
  }

  .group {
    flex: 0 0 132px;
    min-width: 132px;
  }

  .group.transition-group {
    flex-basis: min(330px, calc(100vw - 24px));
    min-width: min(330px, calc(100vw - 24px));
  }

  .group.export-preset-group {
    flex-basis: 190px;
    min-width: 190px;
  }

  .controls > button {
    flex: 0 0 auto;
    align-self: flex-end;
    white-space: nowrap;
  }

  .controls .spacer {
    display: none;
  }

  .export-local-status {
    flex: 0 0 180px;
    align-self: center;
  }

  .theta-row {
    gap: 6px;
  }

  .julia-strip {
    flex-wrap: wrap;
    gap: 6px 10px;
    padding: 6px 10px;
  }

  .julia-hint {
    width: 100%;
    margin-left: 0;
  }

  .stage,
  .stage.points-collapsed {
    grid-template-columns: minmax(0, 1fr);
    grid-template-rows: minmax(240px, 1fr) auto;
  }

  .points {
    border-left: none;
    border-top: 1px solid var(--rule);
    height: min(46dvh, 360px);
    padding: 12px 12px 12px 34px;
  }

  .points.collapsed {
    height: 32px;
    padding: 4px 0;
  }

  .points-toggle {
    top: 6px;
    left: 6px;
  }

  .points.collapsed .points-toggle {
    margin: 0 6px;
  }

  .dual-pane {
    grid-template-columns: 1fr;
    grid-template-rows: 1fr 1fr;
  }

  .pane {
    border-right: none;
    border-bottom: 1px solid var(--rule);
  }

  .pane:last-child {
    border-bottom: none;
  }

  .pane-header {
    padding: 5px 8px;
  }

  .pane-meta {
    white-space: normal;
    line-height: 1.35;
  }

  .modal-backdrop {
    align-items: stretch;
    padding: 10px;
  }

  .modal {
    width: 100%;
    max-height: calc(100dvh - 20px);
    overflow: auto;
    padding: 16px;
  }

  .mrow {
    align-items: stretch;
    flex-direction: column;
    gap: 5px;
  }

  .mrow label {
    min-width: 0;
  }

  .mrow input[type="number"] {
    width: 100%;
  }

  .mrow.estimate span {
    white-space: normal;
  }

  .modal-footer {
    justify-content: stretch;
    flex-wrap: wrap;
  }

  .modal-footer button {
    flex: 1 1 120px;
  }

  .preview-grid {
    grid-template-columns: 1fr;
  }

  .custom-panel {
    padding: 10px;
    max-height: 42dvh;
    overflow: auto;
  }

  .custom-row {
    align-items: stretch;
  }

  .custom-row label {
    width: 100%;
  }

  .formula-input,
  .custom-row input,
  .btn-compile {
    flex: 1 1 100%;
    width: 100% !important;
    min-width: 0;
  }

  .custom-item {
    flex-wrap: wrap;
    gap: 6px;
  }

  .cv-name {
    min-width: 0;
  }

  .cv-formula {
    flex-basis: 100%;
    white-space: normal;
    overflow-wrap: anywhere;
  }
}
</style>
