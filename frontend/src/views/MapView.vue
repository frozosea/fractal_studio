<script setup lang="ts">
import { inject, onMounted, ref, watch, computed } from 'vue'
import MapCanvas from '../components/MapCanvas.vue'
import SpecialPointList from '../components/SpecialPointList.vue'
import TransitionLegEditor from '../components/TransitionLegEditor.vue'
import {
  api, VARIANTS, METRICS, COLORMAPS, VARIANT_LABELS,
  type Metric, type ColorMap, type SpecialPoint,
  type SpecialPointEnumResult,
  type VideoExportResponse, type VideoPreviewResponse, type RunProgress, type RunStatusResponse, type CustomVariant,
  type LnMapColorMode, type TransitionLegInput, type TransitionVideoMode,
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
  mandel_ship_agree:  { en: 'Compare with Mandelbrot', zh: '与 Mandelbrot 对比' },
}

const COLORMAP_LABELS: Record<string, { en: string; zh: string }> = {
  classic_cos: { en: 'Classic Cos', zh: '经典余弦' },
  mod17:       { en: 'Mod-17',      zh: 'Mod-17' },
  hsv_wheel:   { en: 'HSV Wheel',   zh: 'HSV 色轮' },
  tri765:      { en: 'Tri-765',     zh: 'Tri-765' },
  grayscale:   { en: 'Grayscale',   zh: '灰度' },
  hs_rainbow:  { en: 'HS Rainbow',  zh: '隐结构彩虹' },
  inferno:     { en: 'Inferno',     zh: 'Inferno 热阶' },
  viridis:     { en: 'Viridis',     zh: 'Viridis 感知' },
  twilight:    { en: 'Twilight',    zh: 'Twilight 循环' },
  ember_blue:  { en: 'Ember Blue',  zh: '蓝焰' },
  spectral1530:{ en: 'Spectral-1530', zh: '光谱色环 1530' },
}

type LocalizedText = { en: string; zh: string }
type ColorMapInfo = {
  key: ColorMap
  label: LocalizedText
  summary: LocalizedText
  bestFor: LocalizedText
  cost: LocalizedText
}
type LnMapColorModeInfo = {
  key: LnMapColorMode
  label: LocalizedText
  summary: LocalizedText
  bestFor: LocalizedText
  cost: LocalizedText
}

const COLORMAP_OPTIONS: ColorMapInfo[] = [
  {
    key: 'classic_cos',
    label: COLORMAP_LABELS.classic_cos,
    summary: {
      en: 'Legacy three-channel cosine with many rapid color cycles across escape time.',
      zh: '旧版三通道余弦染色，在逃逸时间上有很多快速色彩周期。',
    },
    bestFor: {
      en: 'Finding fine escape bands quickly and matching older renders.',
      zh: '适合快速看细密逃逸层，也适合和旧图保持一致。',
    },
    cost: { en: 'Fast; highly saturated and intentionally busy.', zh: '很快；高饱和、纹理会比较密。' },
  },
  {
    key: 'mod17',
    label: COLORMAP_LABELS.mod17,
    summary: {
      en: 'Discrete modulo coloring that exposes integer iteration steps.',
      zh: '离散取模染色，直接暴露整数迭代层级。',
    },
    bestFor: {
      en: 'Debugging iteration counts and spotting periodic plateaus.',
      zh: '适合调试迭代次数、观察周期平台和断层。',
    },
    cost: { en: 'Fast; deliberately banded.', zh: '很快；会故意呈现硬分层。' },
  },
  {
    key: 'hsv_wheel',
    label: COLORMAP_LABELS.hsv_wheel,
    summary: {
      en: 'Full hue wheel cycling through escape time.',
      zh: '沿逃逸时间循环完整 HSV 色轮。',
    },
    bestFor: {
      en: 'Separating neighboring bands with strong hue contrast.',
      zh: '适合用强色相差异分开相邻逃逸层。',
    },
    cost: { en: 'Fast; vivid but not perceptually uniform.', zh: '很快；鲜艳但感知亮度不均匀。' },
  },
  {
    key: 'tri765',
    label: COLORMAP_LABELS.tri765,
    summary: {
      en: 'Three-leg RGB ramp with crisp transitions.',
      zh: '三段 RGB 斜坡，颜色转折比较利落。',
    },
    bestFor: {
      en: 'High-contrast exploratory renders where hard contours are useful.',
      zh: '适合需要硬轮廓、高对比的探索性渲染。',
    },
    cost: { en: 'Fast; can look synthetic.', zh: '很快；视觉上会偏工程感。' },
  },
  {
    key: 'grayscale',
    label: COLORMAP_LABELS.grayscale,
    summary: {
      en: 'Single-channel brightness map from escape value.',
      zh: '把逃逸值映射成单通道亮度。',
    },
    bestFor: {
      en: 'Reading structure without hue distraction or preparing masks.',
      zh: '适合不想被色相干扰地看结构，或准备 mask。',
    },
    cost: { en: 'Fast; fewer visual channels.', zh: '很快；可用视觉通道较少。' },
  },
  {
    key: 'hs_rainbow',
    label: COLORMAP_LABELS.hs_rainbow,
    summary: {
      en: 'Legacy hidden-structure rainbow for HS metrics; escape falls back to classic cosine.',
      zh: '为隐结构指标准备的旧版彩虹；逃逸时间下会退回经典余弦。',
    },
    bestFor: {
      en: 'Min/max/envelope and pairwise metrics rather than escape-time views.',
      zh: '更适合 min/max/envelope 和 pairwise 指标，不适合普通逃逸时间。',
    },
    cost: { en: 'Fast; metric-oriented.', zh: '很快；偏指标视图。' },
  },
  {
    key: 'inferno',
    label: COLORMAP_LABELS.inferno,
    summary: {
      en: 'Dark-to-hot perceptual ramp from near-black purple through ember orange to pale yellow.',
      zh: '从近黑紫色到橙红再到浅黄的感知热阶。',
    },
    bestFor: {
      en: 'Final stills and videos where depth should read as heat or exposure.',
      zh: '适合最终截图和视频，让深浅关系像热度/曝光一样读出来。',
    },
    cost: { en: 'Fast gradient; smoother and less noisy than cyclic palettes.', zh: '快速渐变；比循环色表更平滑、不那么躁。' },
  },
  {
    key: 'viridis',
    label: COLORMAP_LABELS.viridis,
    summary: {
      en: 'Perceptual purple-blue-green-yellow ramp with stable brightness ordering.',
      zh: '紫蓝到绿黄的感知均匀色表，亮度排序稳定。',
    },
    bestFor: {
      en: 'Analysis screenshots where small numeric differences should remain legible.',
      zh: '适合分析截图，让细小数值差异更容易读。',
    },
    cost: { en: 'Fast gradient; intentionally restrained.', zh: '快速渐变；颜色克制。' },
  },
  {
    key: 'twilight',
    label: COLORMAP_LABELS.twilight,
    summary: {
      en: 'Cyclic dusk palette that returns to its starting color, useful for wrapped phase-like bands.',
      zh: '循环暮光色表，末端回到起点，适合周期/相位感的层纹。',
    },
    bestFor: {
      en: 'Smooth coloring, cyclic ln-map mappings, and contours that should not have a hard endpoint.',
      zh: '适合 smooth coloring、循环 ln-map 映射，或不希望色表端点突兀的轮廓。',
    },
    cost: { en: 'Fast gradient; subtler than HSV wheel.', zh: '快速渐变；比 HSV 色轮更柔和。' },
  },
  {
    key: 'ember_blue',
    label: COLORMAP_LABELS.ember_blue,
    summary: {
      en: 'Blue shadow to cyan edge to ember highlight, tuned for boundary contrast.',
      zh: '从蓝色暗部到青色边缘再到暖色高光，偏边界对比。',
    },
    bestFor: {
      en: 'Fractal edges, frontier-style ln-map modes, and dramatic local exports.',
      zh: '适合边界、frontier 类 ln-map 模式，以及更有戏剧性的本地导出。',
    },
    cost: { en: 'Fast gradient; deliberately stylized.', zh: '快速渐变；风格化更明显。' },
  },
  {
    key: 'spectral1530',
    label: COLORMAP_LABELS.spectral1530,
    summary: {
      en: 'Fully-saturated 1530-step cyclic hue wheel (G→C→B→P→R→Y→G), twice the period of Tri-765 and seamless when wrapped.',
      zh: '全饱和 1530 步循环色环（绿→青→蓝→品→红→黄→绿），周期是 Tri-765 的两倍，循环无缝。',
    },
    bestFor: {
      en: 'The periodic Global-CDF ln-map mode — richest band colors; pairs with a black→green opening.',
      zh: '搭配周期性「全局 CDF」ln-map 模式：色带最丰富；开场从纯黑过渡到绿色。',
    },
    cost: { en: 'Fast; integer hue ramps, no gradient table.', zh: '很快；整数色相渐变，无渐变表。' },
  },
]

function colorMapInfo(mode: ColorMap): ColorMapInfo {
  return COLORMAP_OPTIONS.find(m => m.key === mode) ?? COLORMAP_OPTIONS[0]
}

const LN_MAP_COLOR_MODE_OPTIONS: LnMapColorModeInfo[] = [
  {
    key: 'escape',
    label: { en: 'Escape · original', zh: 'Escape · 原始' },
    summary: {
      en: 'Uses the raw escape iteration directly, preserving the traditional band structure and the fastest SIMD/CUDA paths.',
      zh: '直接使用原始逃逸迭代数，保留传统分层纹理，也保留最快的 SIMD/CUDA 路径。',
    },
    bestFor: {
      en: 'Baseline renders, speed tests, and cases where exact legacy color behavior matters.',
      zh: '适合做基准、速度测试，或需要和旧版逃逸时间染色保持一致的时候。',
    },
    cost: { en: 'Fastest; no whole-strip statistics.', zh: '最快；不需要整张 strip 统计。' },
  },
  {
    key: 'hist_eq',
    label: { en: 'Global CDF · periodic bands', zh: '全局 CDF · 周期色带' },
    summary: {
      en: 'Equalizes escaped-pixel iterations to a global rank, then cycles the palette with cycles ≈ log2(magnification) × cycles/octave, so every depth keeps sharp bands. A black→green lead-in opens the zoom.',
      zh: '把已发散像素的迭代均衡成全局秩，再以「≈log2(放大倍率) × 每倍频周期数」周期性循环调色板，让每个深度都有锐利色带；开场从纯黑过渡到绿色。',
    },
    bestFor: {
      en: 'Long zoom videos that should stay crisp at every depth instead of merging into a smooth ramp. Pairs with the Spectral-1530 palette.',
      zh: '适合长 zoom 视频，让每个深度都保持锐利、不糊成平滑渐变。建议搭配 Spectral-1530 色环。',
    },
    cost: { en: 'OpenMP fp64 plus one global histogram; shared with the final frame.', zh: 'OpenMP fp64，加一次全局直方图；与最终帧共用。' },
  },
  {
    key: 'row_eq',
    label: { en: 'Row CDF · local contrast', zh: '逐行 CDF · 局部对比' },
    summary: {
      en: 'Equalizes each ln-radius row independently, so every depth slice spends the full palette on its local escape distribution.',
      zh: '每个 ln 半径行单独做均衡化，让每个深度切片都把完整色带用在本层的逃逸分布上。',
    },
    bestFor: {
      en: 'Revealing faint angular filaments in deep strips; less faithful to global escape-time scale.',
      zh: '适合挖出深层 strip 中很淡的角向丝状结构；但它会弱化全局逃逸时间尺度。',
    },
    cost: { en: 'OpenMP fp64 plus per-row sorting.', zh: 'OpenMP fp64，每行额外排序。' },
  },
  {
    key: 'log_lift',
    label: { en: 'Log lift · soft early detail', zh: '对数拉伸 · 柔和浅层' },
    summary: {
      en: 'Applies a log curve to normalized escape time, expanding low iteration differences and compressing late escapes without using statistics.',
      zh: '对归一化逃逸时间做对数曲线，放大低迭代差异、压缩高迭代区域，不依赖直方图统计。',
    },
    bestFor: {
      en: 'Smooth explanatory renders and previews where histogram flicker would be distracting.',
      zh: '适合比较柔和的说明性渲染，或者不希望统计均衡化带来跳动感的预览。',
    },
    cost: { en: 'OpenMP fp64; no histogram memory.', zh: 'OpenMP fp64；不需要直方图内存。' },
  },
  {
    key: 'bands',
    label: { en: 'Iso bands · contour readable', zh: '等值色带 · 轮廓可读' },
    summary: {
      en: 'Blends global CDF color with broad and fine periodic bands, turning escape-time changes into visible contour lines.',
      zh: '把全局 CDF 颜色和粗/细周期色带混合，把逃逸时间变化转成更可读的等值轮廓。',
    },
    bestFor: {
      en: 'Studying shells, wakes, and repeated structures where contour separation matters more than smoothness.',
      zh: '适合观察壳层、尾迹、重复结构；更强调轮廓分离，不追求完全平滑。',
    },
    cost: { en: 'OpenMP fp64 plus one global histogram.', zh: 'OpenMP fp64，加一次全局直方图。' },
  },
  {
    key: 'frontier',
    label: { en: 'Frontier glow · edge enhanced', zh: '边界辉光 · 梯度增强' },
    summary: {
      en: 'Uses the global CDF as a base, then brightens places where neighboring escape ranks change quickly.',
      zh: '以全局 CDF 为底，再把邻域逃逸秩变化快的位置提亮，突出边界和脉络。',
    },
    bestFor: {
      en: 'Final videos and stills that need crisp boundaries around filaments and minibrot edges.',
      zh: '适合最终视频或截图，尤其是想让细丝、边界、小 Mandelbrot 轮廓更锋利的时候。',
    },
    cost: { en: 'OpenMP fp64, global histogram, and a small neighbor-gradient pass.', zh: 'OpenMP fp64、全局直方图，以及一次邻域梯度增强。' },
  },
]

function lnMapColorModeInfo(mode: LnMapColorMode): LnMapColorModeInfo {
  return LN_MAP_COLOR_MODE_OPTIONS.find(m => m.key === mode) ?? LN_MAP_COLOR_MODE_OPTIONS[0]
}

const status = inject<StatusState>('status')!
const localExportMode = computed(() => api.isLocalBrowserAccess())
const pngExportStatus = ref('')
const pngExportBusy = ref(false)

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
import { type BigDec, bdFromString, bdFromNumber, bdAddNumber, bdToString, bdToNumber } from '../bigdec'

const centerRePrecise = ref<BigDec>(bdFromString('-0.75'))
const centerImPrecise = ref<BigDec>(bdFromString('0'))
// Bumped on non-incremental center jumps (typed coordinates, imports, reset)
// so the canvases drop their offset-space interaction state.
const mapViewEpoch = ref(0)
const juliaViewEpoch = ref(0)
const centerRe   = ref(-0.75)
const centerIm   = ref( 0.0)
const scale      = ref( 3.0)
const iterations = ref(1024)
const viewportReInput = ref('')
const viewportImInput = ref('')
const viewportZoomInput = ref('')
const activeViewportInput = ref<'' | 're' | 'im' | 'zoom'>('')

const variant  = ref<string>('mandelbrot')  // Variant literal or "custom:HASH"
const metric   = ref<Metric>('escape')
const colorMap = ref<ColorMap>('classic_cos')
const smooth   = ref(false)
const mapColorMode = ref<'direct' | 'eq_full' | 'eq_center'>('direct')  // live equalized preview
const cyclesPerOctave = ref(1)  // band density for equalized modes
const pairwiseCap = ref(64)
const rotationDeg = ref(0)

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
      customCompileMsg.value = r.error ?? (lang.value === 'en' ? 'compile failed' : '编译失败')
    }
  } catch (e: any) {
    customCompileMsg.value = e?.message ?? (lang.value === 'en' ? 'error' : '错误')
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
const transitionMode = ref<'pair' | 'multi'>('pair')
const transitionLegs = ref<TransitionLegInput[]>([
  { variant: 'mandelbrot', weight: 1 },
  { variant: 'burning_ship', weight: 1 },
  { variant: 'tricorn', weight: 0.65 },
])
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
const multiTransitionActive = computed(() => transitionOn.value && transitionMode.value === 'multi')
const activeTransitionLegs = computed(() => {
  const cleaned = transitionLegs.value
    .map(leg => ({
      variant: leg.variant,
      weight: Number.isFinite(leg.weight) ? Math.max(0, Math.min(1, leg.weight)) : 0,
    }))
    .filter(leg => leg.weight > 0)
    .slice(0, 4)
  if (cleaned.length) return cleaned
  return [{ variant: transitionLegs.value[0]?.variant ?? 'mandelbrot', weight: 1 }]
})
const transitionVariantIds = computed(() => activeTransitionLegs.value.map(leg => String(leg.variant)))
const transitionWeights = computed(() => activeTransitionLegs.value.map(leg => leg.weight))
const transitionSummary = computed(() => {
  if (!transitionOn.value) return ''
  if (!multiTransitionActive.value) return `${transitionFrom.value} -> ${transitionTo.value}`
  return activeTransitionLegs.value.map(leg => `${String(leg.variant)}:${leg.weight.toFixed(2)}`).join(' + ')
})

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
    specialPointVariantHint.value = lang.value === 'en'
      ? `Also visible in variants: ${compatible.join(', ')}`
      : `在以下变体中也可见：${compatible.join('、')}`
  } else if (currentBackend && currentBackend !== 'Mandelbrot' && specialPointExistsInVariant(p, currentBackend)) {
    specialPointVariantHint.value = lang.value === 'en'
      ? `Retained Mandelbrot special point for ${specialPointVariantLabel(currentBackend)}`
      : `为 ${specialPointVariantLabel(currentBackend)} 保留的曼德布罗特特殊点`
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
// Arbitrary-precision julia-pane center — without it, deep julia zooms send
// no center strings and the backend cannot use perturbation.
const jCenterRePrecise = ref<BigDec>(bdFromString('0'))
const jCenterImPrecise = ref<BigDec>(bdFromString('0'))

// Julia c input state
const juliaCReInput = ref('')
const juliaCImInput = ref('')
const activeJuliaInput = ref<'' | 'cre' | 'cim'>('')

function syncJuliaCInputs(force = false) {
  if (force || activeJuliaInput.value !== 'cre')
    juliaCReInput.value = juliaRe.value.toPrecision(15)
  if (force || activeJuliaInput.value !== 'cim')
    juliaCImInput.value = juliaIm.value.toPrecision(15)
}

function commitJuliaCInput(kind: 'cre' | 'cim') {
  const raw = (kind === 'cre' ? juliaCReInput.value : juliaCImInput.value).trim()
  const next = Number(raw)
  if (!Number.isFinite(next)) { syncJuliaCInputs(true); return }
  if (kind === 'cre') juliaRe.value = next
  else                juliaIm.value = next
  syncJuliaCInputs(true)
}

function finishJuliaCInput(kind: 'cre' | 'cim') {
  commitJuliaCInput(kind)
  activeJuliaInput.value = ''
}

watch([juliaRe, juliaIm], () => syncJuliaCInputs(), { immediate: true })

// Octave count (倍频数) of the current zoom: log2 of the magnification relative to the
// full-set view (height 4 — the |c|≤2 bounding disk, the same reference the ln-map
// equalization uses). Grows by 1 for every factor-of-2 zoom-in.
const ZOOM_BASE_SCALE = 4
const zoomOctaves = computed(() => {
  const s = juliaOn.value ? jScale.value : scale.value
  return (s > 0 && Number.isFinite(s)) ? Math.log2(ZOOM_BASE_SCALE / s) : 0
})

function formatViewportNumber(value: number, kind: 're' | 'im' | 'zoom'): string {
  if (!Number.isFinite(value)) return ''
  if (kind === 'zoom') {
    const av = Math.abs(value)
    return av >= 1e-4 && av < 1e6 ? value.toPrecision(12) : value.toExponential(12)
  }
  return value.toPrecision(17)
}

function syncViewportInputs(force = false) {
  // Show the full-precision center (the doubles quantize at ~17 digits, and
  // committing a truncated display back would displace deep-zoom views).
  if (force || activeViewportInput.value !== 're') {
    viewportReInput.value = bdToString(centerRePrecise.value)
  }
  if (force || activeViewportInput.value !== 'im') {
    viewportImInput.value = bdToString(centerImPrecise.value)
  }
  if (force || activeViewportInput.value !== 'zoom') {
    viewportZoomInput.value = formatViewportNumber(scale.value, 'zoom')
  }
}

function commitViewportInput(kind: 're' | 'im' | 'zoom') {
  const raw = (kind === 're' ? viewportReInput.value : kind === 'im' ? viewportImInput.value : viewportZoomInput.value).trim()
  // Focus-then-blur without an edit must not touch the precise state.
  if (kind === 're' && raw === bdToString(centerRePrecise.value)) { syncViewportInputs(true); return }
  if (kind === 'im' && raw === bdToString(centerImPrecise.value)) { syncViewportInputs(true); return }
  const next = Number(raw)
  const valid = Number.isFinite(next) && (kind !== 'zoom' || next > 0)
  if (!valid) {
    syncViewportInputs(true)
    return
  }
  if (kind === 're') {
    centerRe.value = next
    centerRePrecise.value = bdFromString(raw)
    mapViewEpoch.value++
  } else if (kind === 'im') {
    centerIm.value = next
    centerImPrecise.value = bdFromString(raw)
    mapViewEpoch.value++
  } else {
    scale.value = Math.max(1e-300, next)
  }
  syncViewportInputs(true)
}

function finishViewportInput(kind: 're' | 'im' | 'zoom') {
  commitViewportInput(kind)
  activeViewportInput.value = ''
}

function commitViewportInputOnEnter(event: KeyboardEvent, kind: 're' | 'im' | 'zoom') {
  commitViewportInput(kind)
  ;(event.target as HTMLInputElement | null)?.blur()
}

// Left-canvas click: pick julia c AND recenter left map
function onPickJulia(pos: { re: number; im: number }) {
  juliaRe.value = pos.re
  juliaIm.value = pos.im
  centerRe.value = pos.re
  centerIm.value = pos.im
  centerRePrecise.value = bdFromNumber(pos.re)
  centerImPrecise.value = bdFromNumber(pos.im)
  mapViewEpoch.value++
}

function onJuliaViewport(v: { centerRe: number; centerIm: number; scale: number; deltaRe?: number; deltaIm?: number }) {
  if (v.deltaRe !== undefined && v.deltaIm !== undefined) {
    jCenterRePrecise.value = bdAddNumber(jCenterRePrecise.value, v.deltaRe)
    jCenterImPrecise.value = bdAddNumber(jCenterImPrecise.value, v.deltaIm)
  } else {
    jCenterRePrecise.value = bdFromNumber(v.centerRe)
    jCenterImPrecise.value = bdFromNumber(v.centerIm)
    juliaViewEpoch.value++
  }
  jCenterRe.value = v.centerRe
  jCenterIm.value = v.centerIm
  jScale.value    = v.scale
}

// The viewport being exported / measured: the julia pane when julia mode is on.
const activeScale = () => juliaOn.value ? jScale.value : scale.value
const activeCenterRe = () => juliaOn.value ? jCenterRe.value : centerRe.value
const activeCenterIm = () => juliaOn.value ? jCenterIm.value : centerIm.value
const activeCenterReStr = () =>
  bdToString(juliaOn.value ? jCenterRePrecise.value : centerRePrecise.value)
const activeCenterImStr = () =>
  bdToString(juliaOn.value ? jCenterImPrecise.value : centerImPrecise.value)

// ── Engine / scalar ───────────────────────────────────────────────────────────
const engineMode = ref<'auto' | 'openmp' | 'avx2' | 'avx512' | 'cuda' | 'hybrid'>('auto')
const scalarMode = ref<'auto' | 'fp32' | 'fp64' | 'fp80' | 'fp128' | 'fx64'>('auto')

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
watch([centerRe, centerIm, scale, centerRePrecise, centerImPrecise],
      () => syncViewportInputs(), { immediate: true })

onMounted(() => {
  loadCustomVariants()

  const pending = sessionStorage.getItem('fs_pending_center')
  if (pending) {
    try {
      const c = JSON.parse(pending)
      if (typeof c.re === 'number' && typeof c.im === 'number') {
        centerRe.value = c.re
        centerIm.value = c.im
        centerRePrecise.value = typeof c.reStr === 'string' ? bdFromString(c.reStr) : bdFromNumber(c.re)
        centerImPrecise.value = typeof c.imStr === 'string' ? bdFromString(c.imStr) : bdFromNumber(c.im)
        mapViewEpoch.value++
        if (typeof c.scale === 'number' && Number.isFinite(c.scale) && c.scale > 0) {
          scale.value = c.scale
        }
      }
    } catch {}
    sessionStorage.removeItem('fs_pending_center')
  }
})

function onViewportChange(v: { centerRe: number; centerIm: number; scale: number; deltaRe?: number; deltaIm?: number }) {
  if (v.deltaRe !== undefined && v.deltaIm !== undefined) {
    centerRePrecise.value = bdAddNumber(centerRePrecise.value, v.deltaRe)
    centerImPrecise.value = bdAddNumber(centerImPrecise.value, v.deltaIm)
  } else {
    centerRePrecise.value = bdFromNumber(v.centerRe)
    centerImPrecise.value = bdFromNumber(v.centerIm)
    mapViewEpoch.value++
  }
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
    jCenterRePrecise.value = bdFromString('0')
    jCenterImPrecise.value = bdFromString('0')
    jScale.value    = 4.0
    juliaViewEpoch.value++
    return
  }
  centerRe.value = 0.0
  centerIm.value = 0.0
  centerRePrecise.value = bdFromString('0')
  centerImPrecise.value = bdFromString('0')
  scale.value    = 4.0
  mapViewEpoch.value++
}

function onImportPoint(p: SpecialPoint | SpecialPointEnumResult) {
  centerRe.value = 're' in p ? p.re : p.real
  centerIm.value = 'im' in p ? p.im : p.imag
  centerRePrecise.value = bdFromNumber(centerRe.value)
  centerImPrecise.value = bdFromNumber(centerIm.value)
  mapViewEpoch.value++
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
  if (juliaOn.value) {
    juliaRe.value = p.re
    juliaIm.value = p.im
  }
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
const showExportFrame = ref(false)

const pngPreset = computed(() =>
  EXPORT_PRESETS.find(p => p.key === pngPresetKey.value) ?? EXPORT_PRESETS[0]
)
const videoPreset = computed(() =>
  EXPORT_PRESETS.find(p => p.key === videoPresetKey.value) ?? EXPORT_PRESETS[0]
)

async function exportPng() {
  pngExportStatus.value = ''
  pngExportBusy.value = true
  try {
    const resp = await api.mapRender({
      taskType:   'still_export',
      localExport: localExportMode.value,
      centerRe:   activeCenterRe(),
      centerIm:   activeCenterIm(),
      centerReStr: activeCenterReStr(),
      centerImStr: activeCenterImStr(),
      scale:      activeScale(),
      width:      pngPreset.value.width,
      height:     pngPreset.value.height,
      iterations: iterations.value,
      variant:    variant.value,
      metric:     metric.value,
      colorMap:   colorMap.value,
      smooth:     smooth.value,
      colorMode:  mapColorMode.value,
      cyclesPerOctave: cyclesPerOctave.value,
      pairwiseCap: pairwiseCap.value,
      engine:     engineMode.value,
      scalarType: scalarMode.value,
      julia:      juliaOn.value,
      juliaRe:    juliaRe.value,
      juliaIm:    juliaIm.value,
      transitionTheta: transitionOn.value ? transitionThetaMilliDeg.value * Math.PI / (180 * THETA_SCALE) : undefined,
      transitionThetaMilliDeg: transitionOn.value ? transitionThetaMilliDeg.value : undefined,
      transitionFrom:  transitionOn.value ? transitionFrom.value : undefined,
      transitionTo:    transitionOn.value ? transitionTo.value : undefined,
      transitionVariants: multiTransitionActive.value ? transitionVariantIds.value : undefined,
      transitionWeights:  multiTransitionActive.value ? transitionWeights.value : undefined,
      rotationDeg:     rotationDeg.value || undefined,
    }) as any
    if (localExportMode.value && resp.localPath) {
      pngExportStatus.value = `${lang.value === 'en' ? 'saved locally' : '已保存到本地'} · ${resp.localPath}`
      status.message = pngExportStatus.value
      return
    }
    window.open(api.artifactDownloadUrl(resp.artifactId), '_blank')
  } catch (e: any) {
    pngExportStatus.value = (lang.value === 'en' ? 'failed: ' : '失败：') + (e?.data?.error || e?.message || e)
    console.error('export PNG failed:', e?.data?.error ?? e)
  } finally {
    pngExportBusy.value = false
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
const exportLnMapColorMode = ref<LnMapColorMode>('escape')
const exportCyclesPerOctave = ref(0.5)
const exportW         = ref(1920)
const exportH         = ref(1080)
const exportBusy      = ref(false)
const exportPreviewBusy = ref(false)
const exportStatus    = ref('')
const exportPreviewStatus = ref('')
const exportResult    = ref<VideoExportResponse | null>(null)
const exportPreviewResult = ref<VideoPreviewResponse | null>(null)
const lnMapPreviewBusy = ref(false)
const lnMapPreviewStatus = ref('')
const lnMapPreviewUrl = ref('')
const lnMapPreviewRunId = ref('')
// Once the user opens the preview, equalization-parameter changes auto re-render at small
// resolution. The escape-count field is cached server-side (keyed by geometry+precision), so
// these recolors reuse it and return fast — real-time parameter tuning.
const lnMapPreviewActive = ref(false)
const lnMapColdRender = ref(false)   // true only while computing the field (not fast recolors)
const LN_PREVIEW_WIDTH_S = 256   // small res: cheap cold field, approximate equalization
let lnPreviewDebounce: ReturnType<typeof setTimeout> | undefined
let lnPreviewRerun = false
const exportJobId     = ref('')
const exportProgress  = ref<RunProgress>({})
const exportDepthDirty = ref(false)
const transitionVideoMode = ref<TransitionVideoMode>('rotation')
const transitionThetaStartDeg = ref(0)
const transitionThetaEndDeg   = ref(180)
const transitionDurationSec   = ref(6)

const exportEstimatedDuration = computed(() =>
  Math.max(0, exportDepth.value) * Math.max(0, exportSecondsPerOctave.value)
)
const exportEstimatedFrames = computed(() =>
  Math.max(2, Math.round(exportEstimatedDuration.value * Math.max(1, exportFps.value)))
)
const visiblePreview = computed(() => exportPreviewResult.value ?? exportResult.value)
const selectedColorMapInfo = computed(() => colorMapInfo(colorMap.value))
const selectedLnMapColorModeInfo = computed(() => lnMapColorModeInfo(exportLnMapColorMode.value))

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
    const colorMode = p.lnMapColorMode ? ` · ${p.lnMapColorMode}` : ''
    return `ln-map ${p.current || 0}/${p.total || 0} rows${colorMode} · octave ${(p.depthOctave || 0).toFixed(2)}/${(p.totalDepthOctaves || 0).toFixed(2)}${etaSuffix(p.estimatedRemainingMs)}`
  }
  if (p.stage === 'video_warp_encode') {
    return `encode ${p.current || 0}/${p.total || 0} frames${etaSuffix(p.estimatedRemainingMs)}`
  }
  if (p.stage === 'final_frame') return `final frame ${p.current || 0}/${p.total || 1}${etaSuffix(p.estimatedRemainingMs)}`
  if (p.stage === 'transition_preview') return `transition preview ${p.current || 0}/2`
  if (p.stage === 'transition_render') {
    if (p.details?.animationMode === 'zoom') {
      const depth = typeof p.details?.depthOctave === 'number' ? p.details.depthOctave : (p.depthOctave || 0)
      return `transition zoom ${p.current || 0}/${p.total || 0} frames · ${depth.toFixed(2)}/${(p.totalDepthOctaves || 0).toFixed(2)} oct${etaSuffix(p.estimatedRemainingMs)}`
    }
    const theta = p.details?.thetaDeg
    const thetaStr = typeof theta === 'number' ? ` · θ=${theta.toFixed(1)}°` : ''
    return `transition rotation ${p.current || 0}/${p.total || 0} frames${thetaStr}${etaSuffix(p.estimatedRemainingMs)}`
  }
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

const transitionEstimatedFrames = computed(() =>
  Math.max(2, Math.round(transitionDurationSec.value * Math.max(1, exportFps.value)))
)

function transitionVideoRequestBase() {
  const fixedThetaDeg = transitionThetaMilliDeg.value / THETA_SCALE
  const zoomMode = transitionVideoMode.value === 'zoom'
  return {
    animationMode: transitionVideoMode.value,
    centerRe:     activeCenterRe(),
    centerIm:     activeCenterIm(),
    centerReStr:  activeCenterReStr(),
    centerImStr:  activeCenterImStr(),
    julia:        juliaOn.value,
    juliaRe:      juliaRe.value,
    juliaIm:      juliaIm.value,
    transitionFrom: transitionFrom.value,
    transitionTo:   transitionTo.value,
    colorMap:     colorMap.value,
    iterations:   Math.max(iterations.value, 2048),
    scale:        activeScale(),
    thetaStartDeg: zoomMode ? fixedThetaDeg : transitionThetaStartDeg.value,
    thetaEndDeg:   zoomMode ? fixedThetaDeg : transitionThetaEndDeg.value,
    thetaDeg:      fixedThetaDeg,
    depthOctaves:  zoomMode ? exportDepth.value : undefined,
    secondsPerOctave: zoomMode ? exportSecondsPerOctave.value : undefined,
    targetScale:   zoomMode && !exportDepthDirty.value && exportDepth.value > 0.05 ? activeScale() : undefined,
    rotationDeg:   rotationDeg.value || undefined,
    durationSec:   zoomMode ? exportEstimatedDuration.value : transitionDurationSec.value,
    fps:          exportFps.value,
    metric:       metric.value === 'escape' ? 'escape' :
                  metric.value === 'min_abs' ? 'min_abs' :
                  metric.value === 'max_abs' ? 'max_abs' :
                  metric.value === 'envelope' ? 'envelope' : 'escape',
    engine:       'auto',
    scalarType:   'auto',
    background:   true,
    localExport:  localExportMode.value,
    width:        exportW.value,
    height:       exportH.value,
  }
}

watch(videoPreset, p => {
  exportW.value = p.width
  exportH.value = p.height
  if (exportModalOpen.value && !exportDepthDirty.value) syncExportDepthToCurrentView()
}, { immediate: true })

watch([exportW, exportH], () => {
  if (exportModalOpen.value && !exportDepthDirty.value) syncExportDepthToCurrentView()
  if (exportModalOpen.value) clearExportPreview()
})

// Periodic (hist_eq) ln-map mode pairs best with the cyclic Spectral-1530 wheel.
// Default to it when the user switches into periodic mode but is still on the
// global default palette — leaving any deliberate palette choice untouched.
watch(exportLnMapColorMode, mode => {
  if (mode === 'hist_eq' && colorMap.value === 'classic_cos') {
    colorMap.value = 'spectral1530'
  }
})

// Live equalized preview pairs best with the cyclic Spectral-1530 wheel, same as the export
// hist_eq mode. Equalization needs per-pixel escape counts, so it only applies to the escape
// metric — fall back to direct coloring otherwise.
watch(mapColorMode, mode => {
  if (mode !== 'direct' && colorMap.value === 'classic_cos') {
    colorMap.value = 'spectral1530'
  }
})
watch(metric, m => {
  if (m !== 'escape') mapColorMode.value = 'direct'
})

// Burning Ship guided-exploration toggle. On → the 'mandel_ship_agree' overlay (renders the
// Burning Ship, greying the regions identical to the Mandelbrot). Off → plain escape time.
const bsExplore = computed({
  get: () => metric.value === 'mandel_ship_agree',
  set: (on: boolean) => { metric.value = on ? 'mandel_ship_agree' : 'escape' },
})

function defaultExportDepthForView() {
  const aspect = Math.max(1e-9, exportW.value / Math.max(1, exportH.value))
  const rMax = Math.sqrt(aspect * aspect + 1)
  const kTopStart = Math.log(4) - Math.log(rMax)
  const kTopEnd = Math.log(Math.max(activeScale(), 1e-300) * 0.5)
  const depth = (kTopStart - kTopEnd) / Math.LN2
  if (!Number.isFinite(depth)) return 20
  return Math.min(1024, Math.max(0.05, depth))
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
    centerRe:     activeCenterRe(),
    centerIm:     activeCenterIm(),
    centerReStr:  activeCenterReStr(),
    centerImStr:  activeCenterImStr(),
    julia:        juliaOn.value,
    juliaRe:      juliaRe.value,
    juliaIm:      juliaIm.value,
    variant:      variant.value,
    colorMap:     colorMap.value,
    lnMapColorMode: exportLnMapColorMode.value,
    lnMapCyclesPerOctave: exportCyclesPerOctave.value,
    iterations:   Math.max(iterations.value, 2048),
    depthOctaves: exportDepth.value,
    fps:          exportFps.value,
    secondsPerOctave: exportSecondsPerOctave.value,
    targetScale:  !exportDepthDirty.value && exportDepth.value > 0.05 ? activeScale() : undefined,
    rotationDeg:  rotationDeg.value || undefined,
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
  exportPreviewStatus.value = lang.value === 'en' ? 'previewing…' : '预览中…'
  exportPreviewResult.value = null
  try {
    if (transitionOn.value) {
      const resp = await api.transitionVideoPreview({
        ...transitionVideoRequestBase(),
        ...previewSizeForExport(),
      })
      exportPreviewResult.value = resp as any
      exportPreviewStatus.value = transitionVideoMode.value === 'zoom'
        ? `${resp.width}×${resp.height} · ${lang.value === 'en' ? 'depth' : '深度'} ${(resp.depthOctaves ?? exportDepth.value).toFixed(2)} · ${resp.generatedMs.toFixed(0)} ms`
        : `${resp.width}×${resp.height} · θ ${resp.thetaStartDeg}°→${resp.thetaEndDeg}° · ${resp.generatedMs.toFixed(0)} ms`
    } else {
      const resp = await api.videoPreview({
        ...videoRequestBase(),
        ...previewSizeForExport(),
      })
      exportPreviewResult.value = resp
      exportPreviewStatus.value = `${resp.width}×${resp.height} · ${lang.value === 'en' ? 'depth' : '深度'} ${resp.depthOctaves.toFixed(2)} · ${resp.generatedMs.toFixed(0)} ms`
    }
  } catch (e: any) {
    exportPreviewStatus.value = (lang.value === 'en' ? 'failed: ' : '失败：') + (e?.data?.error || e?.message || e)
  } finally {
    exportPreviewBusy.value = false
  }
}

// Quick ln-map strip preview so the coloring / band density can be checked before
// committing to the (slow) full video render.
async function runLnMapPreview() {
  lnMapPreviewActive.value = true
  // Coalesce: if a render is already in flight, mark a re-run and let it pick up the latest
  // parameters when it finishes (so dragging a slider doesn't queue a backlog of requests).
  if (lnMapPreviewBusy.value) { lnPreviewRerun = true; return }
  lnMapPreviewBusy.value = true
  const firstField = !lnMapPreviewUrl.value
  lnMapColdRender.value = firstField
  lnMapPreviewStatus.value = firstField
    ? (lang.value === 'en' ? 'computing escape field…' : '计算逃逸次数场…')
    : (lang.value === 'en' ? 'recoloring…' : '重新上色…')
  try {
    const base = videoRequestBase()
    const resp = await api.lnMap({
      centerRe: base.centerRe, centerIm: base.centerIm,
      centerReStr: base.centerReStr, centerImStr: base.centerImStr,
      julia: base.julia, juliaRe: base.juliaRe, juliaIm: base.juliaIm,
      variant: base.variant, colorMap: base.colorMap,
      lnMapColorMode: base.lnMapColorMode,
      lnMapCyclesPerOctave: base.lnMapCyclesPerOctave,
      iterations: base.iterations,
      depthOctaves: base.depthOctaves,
      precisionMode: base.lnMapMode,
      scalarType: base.lnMapScalar,
      widthS: LN_PREVIEW_WIDTH_S,
    })
    lnMapPreviewUrl.value = api.baseUrl + resp.imagePath + '&t=' + Date.now()  // bust img cache
    lnMapPreviewRunId.value = resp.runId
    lnMapPreviewStatus.value = `${resp.widthS}×${resp.heightT} · ${resp.engineUsed || '?'} · ${resp.generatedMs.toFixed(0)} ms`
  } catch (e: any) {
    lnMapPreviewStatus.value = (lang.value === 'en' ? 'failed: ' : '失败：') + (e?.data?.error || e?.message || e)
  } finally {
    lnMapPreviewBusy.value = false
    if (lnPreviewRerun) { lnPreviewRerun = false; runLnMapPreview() }
  }
}

// Debounced real-time recolor: once the preview is open, an equalization/coloring change
// re-renders at small res. Same geometry → the server field cache is hit (fast recolor).
function scheduleLnMapPreview() {
  if (!lnMapPreviewActive.value) return
  if (lnPreviewDebounce) clearTimeout(lnPreviewDebounce)
  lnPreviewDebounce = setTimeout(() => { runLnMapPreview() }, 160)
}

async function runExport() {
  exportBusy.value   = true
  exportStatus.value = lang.value === 'en' ? 'queued…' : '排队中…'
  exportResult.value = null
  exportProgress.value = {}
  try {
    if (transitionOn.value) {
      const resp = await api.transitionVideoExport(transitionVideoRequestBase())
      exportJobId.value = resp.runId
      exportStatus.value = transitionVideoMode.value === 'zoom'
        ? `${resp.runId} · ${resp.frameCount} ${lang.value === 'en' ? 'frames' : '帧'} · ${resp.durationSec.toFixed(2)}s · ${lang.value === 'en' ? 'zoom depth' : 'zoom 深度'} ${(resp.depthOctaves ?? exportDepth.value).toFixed(2)}`
        : `${resp.runId} · ${resp.frameCount} ${lang.value === 'en' ? 'frames' : '帧'} · ${resp.durationSec.toFixed(2)}s · θ ${resp.thetaStartDeg}°→${resp.thetaEndDeg}°`
      await pollVideoExport(resp as any)
    } else {
      const req: Record<string, any> = { ...videoRequestBase() }
      const resp = await api.videoExport(req as any)
      exportJobId.value = resp.runId
      exportStatus.value = `${resp.runId} · ${resp.frameCount} ${lang.value === 'en' ? 'frames' : '帧'} · ${resp.durationSec.toFixed(2)}s`
      await pollVideoExport(resp)
    }
  } catch (e: any) {
    exportStatus.value = (lang.value === 'en' ? 'failed: ' : '失败：') + (e?.data?.error || e?.message || e)
  } finally {
    exportBusy.value = false
  }
}

// Real-time tuning: re-render the preview when an equalization/coloring parameter changes.
// These share the previewed geometry, so the server reuses the cached escape-count field and
// only recolors — fast. (Geometry changes are NOT watched here: the user re-runs the preview
// explicitly to recompute the field, avoiding surprise cold renders while panning.)
watch([exportCyclesPerOctave, colorMap, exportLnMapColorMode], () => scheduleLnMapPreview())

function artifactByName(status: RunStatusResponse, name: string) {
  return status.artifacts.find(a => a.name === name)
}

async function pollVideoExport(initial: VideoExportResponse) {
  for (;;) {
    await new Promise(resolve => setTimeout(resolve, 700))
    const status = await api.runStatus(initial.runId)
    exportProgress.value = status.progress || {}
    if (status.status === 'failed') {
      const msg = status.progress?.errorMessage || (lang.value === 'en' ? 'video export failed' : '视频导出失败')
      exportStatus.value = `${lang.value === 'en' ? 'failed: ' : '失败：'}${status.progress?.failedStage || status.progress?.stage || 'video_export'} · ${msg}`
      return
    }
    if (status.status === 'cancelled') {
      exportStatus.value = lang.value === 'en' ? 'cancelled' : '已取消'
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
      ? (lang.value === 'en'
          ? `completed · ${initial.frameCount} frames · ${video.localPath}`
          : `已完成 · ${initial.frameCount} 帧 · ${video.localPath}`)
      : (lang.value === 'en'
          ? `completed · ${initial.frameCount} frames`
          : `已完成 · ${initial.frameCount} 帧`)
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
        <label :title="lang === 'en'
          ? 'Guided exploration: renders the selected variant and recolours the pixels whose orbit diverges from the plain Mandelbrot (z²+c), so the fold structure unique to this variant stands out. Works for Burning Ship, Celtic, Buffalo, …'
          : '引导式探索：渲染当前变体，并把轨道与普通 Mandelbrot (z²+c) 不同的像素重新上色，突出该变体折叠所独有的结构。适用于 Burning Ship、Celtic、Buffalo 等。'">
          <input type="checkbox" v-model="bsExplore" style="width:auto;margin-right:6px" />
          {{ lang === 'en' ? 'Compare with Mandelbrot' : '与 Mandelbrot 对比' }}
        </label>
      </div>

      <div class="group">
        <label>{{ t('colormap') }}</label>
        <div class="select-help-control">
          <select
            v-model="colorMap"
            :title="`${selectedColorMapInfo.summary[lang]} ${selectedColorMapInfo.bestFor[lang]}`"
            aria-describedby="colormap-tip">
            <option
              v-for="c in COLORMAPS"
              :key="c"
              :value="c"
              :title="`${colorMapInfo(c).summary[lang]} ${colorMapInfo(c).bestFor[lang]} ${colorMapInfo(c).cost[lang]}`">
              {{ COLORMAP_LABELS[c]?.[lang] ?? c }}
            </option>
          </select>
          <div id="colormap-tip" class="select-help-detail" role="tooltip">
            <div class="select-help-title">{{ selectedColorMapInfo.label[lang] }}</div>
            <p>{{ selectedColorMapInfo.summary[lang] }}</p>
            <div>
              <span>{{ lang === 'en' ? 'Best for' : '适合' }}</span>
              {{ selectedColorMapInfo.bestFor[lang] }}
            </div>
            <div>
              <span>{{ lang === 'en' ? 'Cost' : '代价' }}</span>
              {{ selectedColorMapInfo.cost[lang] }}
            </div>
          </div>
        </div>
      </div>

      <div class="group">
        <label>{{ lang === 'en' ? 'Color mode' : '上色模式' }}</label>
        <select
          v-model="mapColorMode"
          :title="lang === 'en'
            ? 'Equalized modes preview the ln-map zoom-video coloring live (escape metric only).'
            : '均衡化模式可实时预览 ln-map 缩放视频的上色（仅限 escape 度量）。'">
          <option value="direct">{{ lang === 'en' ? 'Direct mapping' : '直接映射' }}</option>
          <option value="eq_full" :disabled="metric !== 'escape'">
            {{ lang === 'en' ? 'Equalized (full)' : '均衡化（全图）' }}
          </option>
          <option value="eq_center" :disabled="metric !== 'escape'">
            {{ lang === 'en' ? 'Equalized (center)' : '均衡化（中心加权）' }}
          </option>
        </select>
      </div>

      <div class="group" v-if="mapColorMode !== 'direct'">
        <label>{{ lang === 'en' ? 'Cycles / octave' : '每倍频周期' }}</label>
        <input
          type="number" v-model.number="cyclesPerOctave"
          min="0.05" max="64" step="0.05"
          style="width:5em"
          :title="lang === 'en' ? 'Palette cycles per octave of zoom — higher = denser bands.'
                                 : '每个缩放倍频的调色板周期数 —— 越大条带越密。'" />
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

      <div class="group viewport-edit-group">
        <label>{{ lang === 'en' ? 'viewport' : '视口' }}</label>
        <div class="viewport-edit-row">
          <label class="viewport-field">
            <span>{{ lang === 'en' ? 'Real' : '实部' }}</span>
            <input
              v-model="viewportReInput"
              class="viewport-input mono"
              type="text"
              inputmode="decimal"
              spellcheck="false"
              :title="lang === 'en' ? 'center real' : '中心实部'"
              @focus="activeViewportInput = 're'"
              @blur="finishViewportInput('re')"
              @keydown.enter="commitViewportInputOnEnter($event, 're')" />
          </label>
          <label class="viewport-field">
            <span>{{ lang === 'en' ? 'Imag' : '虚部' }}</span>
            <input
              v-model="viewportImInput"
              class="viewport-input mono"
              type="text"
              inputmode="decimal"
              spellcheck="false"
              :title="lang === 'en' ? 'center imaginary' : '中心虚部'"
              @focus="activeViewportInput = 'im'"
              @blur="finishViewportInput('im')"
              @keydown.enter="commitViewportInputOnEnter($event, 'im')" />
          </label>
          <label class="viewport-field">
            <span>zoom <span class="viewport-oct" :title="lang === 'en' ? 'octaves of zoom (log₂ magnification vs the |c|≤2 full-set view)' : '缩放倍频数（相对 |c|≤2 全集视图的 log₂ 放大倍率）'">· {{ zoomOctaves.toFixed(2) }} oct</span></span>
            <input
              v-model="viewportZoomInput"
              class="viewport-input mono"
              type="text"
              inputmode="decimal"
              spellcheck="false"
              :title="lang === 'en' ? 'viewport vertical span' : '视口垂直跨度'"
              @focus="activeViewportInput = 'zoom'"
              @blur="finishViewportInput('zoom')"
              @keydown.enter="commitViewportInputOnEnter($event, 'zoom')" />
          </label>
        </div>
      </div>

      <div v-if="metric === 'min_pairwise_dist'" class="group">
        <label>{{ lang === 'en' ? 'Orbit buffer size' : '轨道缓冲区大小' }}</label>
        <input type="number" v-model.number="pairwiseCap" min="1" max="1000000" step="64" />
      </div>

      <div class="group transition-group">
        <label>
          <input type="checkbox" v-model="transitionOn" style="width:auto;margin-right:6px" />
          {{ t('transition') }}
        </label>
        <div v-if="transitionOn" class="transition-mode-row">
          <button :class="{ active: transitionMode === 'pair' }" @click="transitionMode = 'pair'">Pair</button>
          <button :class="{ active: transitionMode === 'multi' }" @click="transitionMode = 'multi'">Multi</button>
        </div>
        <div v-if="transitionOn && transitionMode === 'pair'" class="theta-row">
          <button class="theta-loop-btn" @click="nudgeThetaDeg(-15)">−</button>
          <input type="range" min="-180" max="180" step="0.1" v-model.number="thetaDeg" />
          <button class="theta-loop-btn" @click="nudgeThetaDeg(15)">+</button>
          <input class="theta-input num" type="number" min="-180" max="180" step="0.1" :value="thetaDeg.toFixed(1)" @change="setThetaDeg(Number(($event.target as HTMLInputElement).value))" />
          <span class="num">°</span>
        </div>
        <div v-if="transitionOn && transitionMode === 'pair'" class="theta-row theta-snaps">
          <button @click="setThetaDeg(-180)">−180</button>
          <button @click="setThetaDeg(-90)">−90</button>
          <button @click="setThetaDeg(0)">0</button>
          <button @click="setThetaDeg(90)">90</button>
          <button @click="setThetaDeg(180)">180</button>
        </div>
        <div v-if="transitionOn && transitionMode === 'pair'" class="theta-row">
          <select v-model="transitionFrom">
            <option v-for="v in AXIS_TRANSITION_VARIANTS" :key="'from-' + v" :value="v">{{ VARIANT_LABELS[v][lang] }}</option>
          </select>
          <span class="num">→</span>
          <select v-model="transitionTo">
            <option v-for="v in AXIS_TRANSITION_VARIANTS" :key="'to-' + v" :value="v">{{ VARIANT_LABELS[v][lang] }}</option>
          </select>
        </div>
        <TransitionLegEditor
          v-if="transitionOn && transitionMode === 'multi'"
          v-model="transitionLegs"
          :min="1"
          :max="4" />
      </div>

      <div class="group">
        <label>
          <input type="checkbox" v-model="juliaOn" style="width:auto;margin-right:6px" />
          {{ t('julia') }}
        </label>
      </div>

      <div class="group rotation-group">
        <label>{{ lang === 'en' ? 'Rotation' : '旋转' }}</label>
        <div style="display:flex;align-items:center;gap:6px">
          <input type="range" v-model.number="rotationDeg" min="-180" max="180" step="1" style="flex:1;min-width:60px" />
          <input type="number" v-model.number="rotationDeg" min="-180" max="180" step="1" class="num" style="width:60px" />
          <span class="mono" style="font-size:11px">°</span>
        </div>
        <div style="display:flex;gap:4px;margin-top:2px">
          <button v-for="a in [-90, -45, 0, 45, 90]" :key="a" class="snap-btn" @click="rotationDeg = a">{{ a }}°</button>
        </div>
      </div>

      <div class="group">
        <label>{{ t('engine') }}</label>
        <select v-model="engineMode">
          <option value="auto">{{ lang === 'en' ? 'Auto (recommended)' : '自动（推荐）' }}</option>
          <option value="cuda">GPU (CUDA)</option>
          <option value="avx512">CPU AVX-512</option>
          <option value="avx2">CPU AVX-2</option>
          <option value="hybrid">CPU + GPU</option>
          <option value="openmp">{{ lang === 'en' ? 'CPU (basic)' : 'CPU（基础）' }}</option>
        </select>
      </div>

      <div class="group">
        <label>{{ t('scalar') }}</label>
        <select v-model="scalarMode">
          <option value="auto">{{ lang === 'en' ? 'Auto' : '自动' }}</option>
          <option value="fp32">{{ lang === 'en' ? '32-bit (fast)' : '32 位（快速）' }}</option>
          <option value="fp64">{{ lang === 'en' ? '64-bit (standard)' : '64 位（标准）' }}</option>
          <option value="fp80">{{ lang === 'en' ? '80-bit (extended)' : '80 位（扩展）' }}</option>
          <option value="fp128">{{ lang === 'en' ? '128-bit (deep zoom)' : '128 位（深层缩放）' }}</option>
          <option value="fx64">{{ lang === 'en' ? 'Fixed 64-bit' : '定点 64 位' }}</option>
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
        <label style="margin-top:4px;font-size:11px">
          <input type="checkbox" v-model="showExportFrame" style="width:auto;margin-right:4px" />
          {{ lang === 'en' ? 'Composition frame' : '构图框' }}
        </label>
      </div>

      <button @click="resetView" :title="juliaOn ? t('reset_julia') : t('reset')">
        ⌂ {{ juliaOn ? t('reset_julia') : t('reset') }}
      </button>
      <button @click="exportPng" :disabled="pngExportBusy">{{ pngExportBusy ? t('loading') : t('export_png') }}</button>
      <button
        @click="openExportModal"
        :disabled="multiTransitionActive"
        :title="multiTransitionActive ? (lang === 'en' ? 'Multi transition video paths are not enabled yet.' : '多变体 transition 暂未启用视频路径。') : t('export_video')">
        {{ t('export_video') }}
      </button>
      <progress v-if="pngExportBusy" class="png-export-progress"></progress>
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
          <button class="cv-use" @click="variant = cv.variantId; showCustomPanel = false">{{ lang === 'en' ? 'use' : '使用' }}</button>
          <button class="cv-del" @click="deleteCustom(cv.variantId)">{{ t('custom_delete') }}</button>
        </div>
      </div>
    </div>

    <!-- ── Julia info strip ─────────────────────────────────────────────── -->
    <div v-if="juliaOn" class="julia-strip mono">
      <span class="julia-label">c =</span>
      <input class="julia-c-input num" type="text"
        v-model="juliaCReInput"
        @focus="activeJuliaInput = 'cre'"
        @blur="finishJuliaCInput('cre')"
        @keydown.enter="commitJuliaCInput('cre'); ($event.target as HTMLInputElement).blur()" />
      <span class="julia-label">+</span>
      <input class="julia-c-input num" type="text"
        v-model="juliaCImInput"
        @focus="activeJuliaInput = 'cim'"
        @blur="finishJuliaCInput('cim')"
        @keydown.enter="commitJuliaCInput('cim'); ($event.target as HTMLInputElement).blur()" />
      <span class="julia-label">i</span>
      <span class="julia-hint">{{ t('julia_hint') }}</span>
    </div>

    <!-- ── Main stage: dual-pane or single ──────────────────────────────── -->
    <div class="stage" :class="{ 'points-collapsed': pointsCollapsed }">

      <!-- Single-pane mode (no Julia) -->
      <template v-if="!juliaOn">
        <MapCanvas
          :centerRe="centerRe" :centerIm="centerIm" :scale="scale"
          :centerReStr="bdToString(centerRePrecise)" :centerImStr="bdToString(centerImPrecise)"
          :viewEpoch="mapViewEpoch"
          :iterations="iterations" :variant="variant" :metric="metric"
          :colorMap="colorMap" :smooth="smooth" :colorMode="mapColorMode" :cyclesPerOctave="cyclesPerOctave"
          :pairwise-cap="pairwiseCap"
          :transitionTheta="transitionOn ? transitionThetaMilliDeg * Math.PI / (180 * THETA_SCALE) : null"
          :transition-theta-milli-deg="activeTransitionThetaMilliDeg"
          :transitionFrom="transitionFrom" :transitionTo="transitionTo"
          :transitionVariants="multiTransitionActive ? transitionVariantIds : undefined"
          :transitionWeights="multiTransitionActive ? transitionWeights : undefined"
          :engine="engineMode" :scalarType="scalarMode"
          :rotationDeg="rotationDeg"
          :showExportFrame="showExportFrame"
          :exportFrameWidth="pngPreset.width"
          :exportFrameHeight="pngPreset.height"
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
            :title="lang === 'en' ? (pointsCollapsed ? 'Expand root list' : 'Collapse root list') : (pointsCollapsed ? '展开根列表' : '折叠根列表')"
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
                :centerReStr="bdToString(centerRePrecise)" :centerImStr="bdToString(centerImPrecise)"
                :viewEpoch="mapViewEpoch"
                :iterations="iterations" :variant="variant" :metric="metric"
                :colorMap="colorMap" :smooth="smooth" :colorMode="mapColorMode" :cyclesPerOctave="cyclesPerOctave"
                :pairwise-cap="pairwiseCap"
                :transitionTheta="transitionOn ? transitionThetaMilliDeg * Math.PI / (180 * THETA_SCALE) : null"
                :transition-theta-milli-deg="activeTransitionThetaMilliDeg"
                :transitionFrom="transitionFrom" :transitionTo="transitionTo"
                :transitionVariants="multiTransitionActive ? transitionVariantIds : undefined"
                :transitionWeights="multiTransitionActive ? transitionWeights : undefined"
                :engine="engineMode" :scalarType="scalarMode"
                :rotationDeg="rotationDeg"
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
                :centerReStr="bdToString(jCenterRePrecise)" :centerImStr="bdToString(jCenterImPrecise)"
                :viewEpoch="juliaViewEpoch"
                :iterations="iterations" :variant="variant" :metric="metric"
                :colorMap="colorMap" :smooth="smooth" :colorMode="mapColorMode" :cyclesPerOctave="cyclesPerOctave"
                :pairwise-cap="pairwiseCap"
                :transitionTheta="transitionOn ? transitionThetaMilliDeg * Math.PI / (180 * THETA_SCALE) : null"
                :transition-theta-milli-deg="activeTransitionThetaMilliDeg"
                :transitionFrom="transitionFrom" :transitionTo="transitionTo"
                :transitionVariants="multiTransitionActive ? transitionVariantIds : undefined"
                :transitionWeights="multiTransitionActive ? transitionWeights : undefined"
                :julia="true" :juliaRe="juliaRe" :juliaIm="juliaIm"
                :engine="engineMode" :scalarType="scalarMode"
                :rotationDeg="rotationDeg"
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
        <div class="modal" :class="{ 'modal-with-preview': lnMapPreviewUrl }">
          <div class="modal-main">
          <div class="modal-title">
            {{ transitionOn ? (lang === 'en' ? 'Export Transition Video' : '导出 Transition 视频') : juliaOn ? t('export_julia_video') : t('export_video') }}
          </div>
          <div v-if="juliaOn" class="mrow source mono" style="margin-bottom:6px">
            Julia c: {{ juliaRe.toPrecision(8) }} + {{ juliaIm.toPrecision(8) }}i
          </div>
          <div class="modal-body">
            <div v-if="transitionOn" class="mrow">
              <label>{{ lang === 'en' ? 'Transition' : '变换' }}</label>
              <span class="mono">{{ transitionSummary }}</span>
            </div>
            <div v-if="transitionOn" class="mrow">
              <label>{{ lang === 'en' ? 'Video type' : '视频类型' }}</label>
              <div class="transition-mode-row export-mode-row">
                <button :class="{ active: transitionVideoMode === 'rotation' }" @click="transitionVideoMode = 'rotation'">Rotation</button>
                <button :class="{ active: transitionVideoMode === 'zoom' }" @click="transitionVideoMode = 'zoom'">Zoom</button>
              </div>
            </div>
            <div v-if="transitionOn && transitionVideoMode === 'rotation'" class="mrow">
              <label>{{ lang === 'en' ? 'θ start (°)' : 'θ 起始 (°)' }}</label>
              <input type="number" v-model.number="transitionThetaStartDeg" min="-180" max="180" step="1" />
            </div>
            <div v-if="transitionOn && transitionVideoMode === 'rotation'" class="mrow">
              <label>{{ lang === 'en' ? 'θ end (°)' : 'θ 终止 (°)' }}</label>
              <input type="number" v-model.number="transitionThetaEndDeg" min="-180" max="180" step="1" />
            </div>
            <div v-if="transitionOn && transitionVideoMode === 'zoom'" class="mrow">
              <label>{{ lang === 'en' ? 'Fixed θ (°)' : '固定 θ (°)' }}</label>
              <input type="number" v-model.number="thetaDeg" min="-180" max="180" step="1" />
            </div>
            <div v-if="transitionOn && transitionVideoMode === 'rotation'" class="mrow">
              <label>{{ lang === 'en' ? 'Duration (s)' : '时长 (s)' }}</label>
              <input type="number" v-model.number="transitionDurationSec" min="0.5" max="3600" step="0.5" />
            </div>
            <div v-if="!transitionOn || transitionVideoMode === 'zoom'" class="mrow">
              <label>{{ t('video_depth') }}</label>
              <input type="number" v-model.number="exportDepth" min="0.05" max="120" step="0.05" @input="onExportDepthInput" />
            </div>
            <div class="mrow">
              <label>{{ t('video_fps') }}</label>
              <input type="number" v-model.number="exportFps" min="1" max="120" step="1" />
            </div>
            <div v-if="!transitionOn || transitionVideoMode === 'zoom'" class="mrow">
              <label>{{ t('video_seconds_per_octave') }}</label>
              <input type="number" v-model.number="exportSecondsPerOctave" min="0.05" max="60" step="0.05" />
            </div>
            <div v-if="transitionOn && transitionVideoMode === 'rotation'" class="mrow estimate">
              <label>{{ t('video_estimate') }}</label>
              <span class="mono">{{ transitionDurationSec.toFixed(1) }}s · {{ transitionEstimatedFrames }} frames</span>
            </div>
            <div v-else-if="transitionOn && transitionVideoMode === 'zoom'" class="mrow estimate">
              <label>{{ t('video_estimate') }}</label>
              <span class="mono">{{ exportEstimatedDuration.toFixed(2) }}s · {{ exportEstimatedFrames }} frames</span>
            </div>
            <div v-if="!transitionOn" class="mrow estimate">
              <label>{{ t('video_estimate') }}</label>
              <span class="mono">{{ exportEstimatedDuration.toFixed(2) }}s · {{ exportEstimatedFrames }} frames · {{ exportMemoryEstimateMiB.toFixed(0) }} MiB</span>
            </div>
            <div v-if="localExportMode && !transitionOn" class="mrow local-mode-row">
              <label>{{ lang === 'en' ? 'Output' : '输出' }}</label>
              <span class="mono">{{ lang === 'en' ? 'local mode: save to backend runtime/runs, no browser download by default' : '本地模式：默认写入后端 runtime/runs，不走浏览器下载' }}</span>
            </div>
            <div v-if="!transitionOn" class="mrow">
              <label>{{ lang === 'en' ? 'Quality' : '质量' }}</label>
              <select v-model="exportQualityPreset">
                <option value="draft">draft</option>
                <option value="balanced">balanced</option>
                <option value="high">high</option>
                <option value="full">full</option>
              </select>
            </div>
            <div v-if="!transitionOn" class="mrow">
              <label>{{ lang === 'en' ? 'ln-map mode' : 'ln-map 模式' }}</label>
              <select v-model="exportLnMapMode">
                <option value="fast">{{ lang === 'en' ? 'fast · depth layered' : '快速 · 深度分层' }}</option>
                <option value="standard">{{ lang === 'en' ? 'standard · full precision' : '标准 · 全精度' }}</option>
              </select>
            </div>
            <div v-if="!transitionOn" class="mrow color-mode-row">
              <label>{{ lang === 'en' ? 'ln-map color' : 'ln-map 上色' }}</label>
              <div class="color-mode-control">
                <select
                  v-model="exportLnMapColorMode"
                  :title="`${selectedLnMapColorModeInfo.summary[lang]} ${selectedLnMapColorModeInfo.bestFor[lang]}`"
                  aria-describedby="ln-map-color-mode-tip">
                  <option
                    v-for="m in LN_MAP_COLOR_MODE_OPTIONS"
                    :key="m.key"
                    :value="m.key"
                    :title="`${m.summary[lang]} ${m.bestFor[lang]} ${m.cost[lang]}`">
                    {{ m.label[lang] }}
                  </option>
                </select>
                <div id="ln-map-color-mode-tip" class="color-mode-detail" role="tooltip">
                  <div class="color-mode-title">{{ selectedLnMapColorModeInfo.label[lang] }}</div>
                  <p>{{ selectedLnMapColorModeInfo.summary[lang] }}</p>
                  <div>
                    <span>{{ lang === 'en' ? 'Best for' : '适合' }}</span>
                    {{ selectedLnMapColorModeInfo.bestFor[lang] }}
                  </div>
                  <div>
                    <span>{{ lang === 'en' ? 'Cost' : '代价' }}</span>
                    {{ selectedLnMapColorModeInfo.cost[lang] }}
                  </div>
                </div>
              </div>
            </div>
            <div class="mrow" v-if="!transitionOn && exportLnMapColorMode === 'hist_eq'">
              <label>{{ lang === 'en' ? 'Cycles / octave' : '每倍频周期' }}</label>
              <div class="cpo-control"
                   :title="lang === 'en'
                     ? 'Palette cycles per zoom octave. Total ≈ log2(magnification) × this. 1 = literal log-of-magnification; higher = denser bands. Drag to tune the preview live.'
                     : '每个 zoom 倍频的调色板周期数。总周期 ≈ log2(放大倍率) × 此值。1 = 严格“放大倍率的对数”；越大色带越密。拖动可实时调预览。'">
                <input type="range" v-model.number="exportCyclesPerOctave" min="0.1" max="16" step="0.05" class="cpo-slider" />
                <input type="number" v-model.number="exportCyclesPerOctave" min="0.1" max="64" step="0.05" class="cpo-num" />
              </div>
            </div>
            <div v-if="!transitionOn" class="mrow">
              <label>{{ lang === 'en' ? 'ln-map scalar' : 'ln-map 标量' }}</label>
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
            <button v-if="!transitionOn" @click="runLnMapPreview" :disabled="exportBusy || exportPreviewBusy || lnMapPreviewBusy" class="btn-preview">
              {{ lnMapPreviewBusy ? t('loading') : (lang === 'en' ? 'ln-map preview' : 'ln-map 预览') }}
            </button>
            <button @click="runPreview" :disabled="exportBusy || exportPreviewBusy || lnMapPreviewBusy" class="btn-preview">
              {{ exportPreviewBusy ? t('loading') : t('video_preview') }}
            </button>
            <button @click="runExport" :disabled="exportBusy || exportPreviewBusy || lnMapPreviewBusy" class="btn-go">
              {{ exportBusy ? t('loading') : t('video_render') }}
            </button>
          </div>
          <div v-if="lnMapPreviewActive" class="ln-preview-progress">
            <progress v-if="lnMapPreviewBusy && lnMapColdRender"></progress>
            <span v-if="lnMapPreviewStatus" class="modal-status mono">{{ lnMapPreviewStatus }}</span>
          </div>
          <div v-if="exportPreviewStatus" class="modal-status mono">{{ exportPreviewStatus }}</div>
          <div v-if="exportStatus" class="modal-status mono">{{ exportStatus }}</div>
          <div v-if="(exportBusy || exportJobId) && transitionOn" class="progress-stack">
            <div class="progress-row">
              <span>{{ lang === 'en' ? 'preview' : '预览' }}</span>
              <progress :value="progressRatio('transition_preview')" max="1"></progress>
            </div>
            <div class="progress-row">
              <span>{{ lang === 'en' ? 'render' : '渲染' }}</span>
              <progress :value="progressRatio('transition_render')" max="1"></progress>
            </div>
          </div>
          <div v-if="(exportBusy || exportJobId) && !transitionOn" class="progress-stack">
            <div class="progress-row">
              <span>{{ lang === 'en' ? 'final' : '终帧' }}</span>
              <progress :value="progressRatio('final_frame')" max="1"></progress>
            </div>
            <div class="progress-row">
              <span>ln-map</span>
              <progress :value="progressRatio('ln_map')" max="1"></progress>
            </div>
            <div class="progress-row">
              <span>{{ lang === 'en' ? 'encode' : '编码' }}</span>
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
              <div v-if="exportResult.videoLocalPath">{{ lang === 'en' ? 'video:' : '视频：' }} {{ exportResult.videoLocalPath }}</div>
              <div v-if="exportResult.lnMapLocalPath">ln-map: {{ exportResult.lnMapLocalPath }}</div>
              <div v-if="exportResult.finalFrameLocalPath">{{ lang === 'en' ? 'final:' : '终帧：' }} {{ exportResult.finalFrameLocalPath }}</div>
            </div>
            <template v-else>
              <a v-if="exportResult.videoDownloadUrl" :href="api.baseUrl + exportResult.videoDownloadUrl" class="dl-link" download>↓ {{ t('video_download') }}</a>
              <a v-if="exportResult.lnMapDownloadUrl" :href="api.baseUrl + exportResult.lnMapDownloadUrl" class="dl-link" download>↓ ln-map PNG</a>
              <a v-if="exportResult.finalFrameDownloadUrl" :href="api.baseUrl + exportResult.finalFrameDownloadUrl" class="dl-link" download>↓ {{ t('export_png') }}</a>
            </template>
          </div>
          </div><!-- end .modal-main -->
          <div v-if="lnMapPreviewUrl" class="modal-ln-preview">
            <div class="ln-preview-label mono">
              {{ lang === 'en' ? 'preview strip' : '预览条带' }}
            </div>
            <div class="ln-preview-scroll">
              <a :href="lnMapPreviewUrl" download>
                <img :src="lnMapPreviewUrl" alt="" class="ln-preview-img" />
              </a>
            </div>
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

.group.transition-group { min-width: 340px; }
.group.export-preset-group { min-width: 190px; }
.group.viewport-edit-group {
  min-width: 392px;
}
.viewport-edit-row {
  display: grid;
  grid-template-columns: repeat(3, 124px);
  gap: 6px;
}
.viewport-field {
  display: flex;
  flex-direction: column;
  gap: 2px;
}
.viewport-field span {
  color: var(--text-dim);
  font-size: 9px;
  line-height: 1;
  text-transform: uppercase;
  letter-spacing: 0.05em;
}
.viewport-oct {
  color: var(--accent, #6cf);
  font-variant-numeric: tabular-nums;
}
.viewport-input {
  width: 124px;
  font-size: 11px;
}
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

.transition-mode-row {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 4px;
  margin-bottom: 5px;
}

.transition-mode-row button {
  padding: 3px 6px;
  font-size: 10px;
}

.transition-mode-row button.active {
  border-color: var(--accent-edge);
  color: var(--accent);
  background: var(--accent-weak);
}

.export-mode-row {
  margin-bottom: 0;
  min-width: 180px;
}

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

.snap-btn {
  flex: 1;
  min-width: 32px;
  padding: 2px 4px;
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
.julia-label { color: var(--text-dim); letter-spacing: 0.08em; }
.julia-c-input {
  width: 14em; padding: 2px 6px;
  background: var(--input-bg); color: var(--accent); border: 1px solid var(--border);
  border-radius: 3px; font: inherit;
}
.julia-c-input:focus { border-color: var(--accent); outline: none; }
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
  width: min(520px, calc(100vw - 32px));
  padding: 24px 28px;
  display: flex;
  flex-direction: column;
  gap: 14px;
}
.modal.modal-with-preview {
  flex-direction: row;
  width: min(780px, calc(100vw - 32px));
  max-height: calc(100vh - 48px);
  padding: 0;
  gap: 0;
}
.modal.modal-with-preview .modal-main {
  flex: 1;
  min-width: 0;
  padding: 24px 28px;
  display: flex;
  flex-direction: column;
  gap: 14px;
  overflow-y: auto;
}
.modal-ln-preview {
  width: 200px;
  flex-shrink: 0;
  border-left: 1px solid var(--rule);
  display: flex;
  flex-direction: column;
  overflow: hidden;
}
.cpo-control {
  display: flex;
  align-items: center;
  gap: 8px;
  flex: 1;
}
.cpo-slider {
  flex: 1;
  min-width: 90px;
}
.cpo-num {
  width: 64px;
  flex-shrink: 0;
}
.ln-preview-progress {
  display: flex;
  align-items: center;
  gap: 8px;
}
.ln-preview-progress progress {
  width: 120px;
  height: 6px;
}
.ln-preview-label {
  font-size: 10px;
  color: var(--text-dim);
  text-transform: uppercase;
  letter-spacing: 0.06em;
  padding: 8px 10px 4px;
  flex-shrink: 0;
}
.ln-preview-scroll {
  flex: 1;
  overflow-y: auto;
  overflow-x: hidden;
  padding: 0 4px 8px;
}
.ln-preview-img {
  width: 100%;
  height: auto;
  image-rendering: crisp-edges;
  display: block;
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
.mrow.color-mode-row {
  align-items: center;
}
.color-mode-control,
.select-help-control {
  flex: 1;
  min-width: 0;
  position: relative;
}
.color-mode-control select,
.select-help-control select {
  width: 100%;
}
.color-mode-detail,
.select-help-detail {
  position: absolute;
  top: calc(100% + 7px);
  right: 0;
  z-index: 20;
  width: min(360px, calc(100vw - 48px));
  max-width: 100%;
  opacity: 0;
  transform: translateY(-3px);
  pointer-events: none;
  transition: opacity 120ms ease, transform 120ms ease;
  background: var(--panel);
  border: 1px solid var(--rule);
  border-left: 2px solid var(--accent-edge);
  padding: 9px 10px 9px 11px;
  box-shadow: 0 12px 28px rgba(0, 0, 0, 0.35);
  font-size: 10px;
  line-height: 1.45;
  color: var(--text-dim);
}
.select-help-detail {
  width: min(300px, calc(100vw - 48px));
  max-width: none;
}
.color-mode-control:hover .color-mode-detail,
.color-mode-control:focus-within .color-mode-detail,
.select-help-control:hover .select-help-detail,
.select-help-control:focus-within .select-help-detail {
  opacity: 1;
  transform: translateY(0);
}
.color-mode-title,
.select-help-title {
  color: var(--accent);
  font-family: var(--mono);
  font-size: 10px;
  text-transform: uppercase;
  letter-spacing: 0.05em;
  margin-bottom: 3px;
}
.color-mode-detail p,
.select-help-detail p {
  margin: 0 0 5px;
}
.color-mode-detail div,
.select-help-detail div {
  margin-top: 3px;
}
.color-mode-detail span,
.select-help-detail span {
  color: var(--text);
  margin-right: 6px;
}
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
.png-export-progress {
  width: 80px;
  height: 7px;
  accent-color: var(--accent);
  vertical-align: middle;
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





































@media (max-width: 760px), ((pointer: coarse) and (max-width: 1200px)), ((any-pointer: coarse) and (max-width: 1200px)), ((min-width: 761px) and (max-width: 1200px) and (orientation: landscape)) {
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
    flex-basis: min(360px, calc(100vw - 24px));
    min-width: min(360px, calc(100vw - 24px));
  }

  .group.export-preset-group {
    flex-basis: 190px;
    min-width: 190px;
  }

  .group.viewport-edit-group {
    flex-basis: 392px;
    min-width: 392px;
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

  .modal.modal-with-preview {
    flex-direction: column;
    width: 100%;
    padding: 0;
  }
  .modal.modal-with-preview .modal-main {
    padding: 16px;
  }
  .modal-ln-preview {
    width: 100%;
    border-left: none;
    border-top: 1px solid var(--rule);
    max-height: 300px;
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

@media (min-width: 761px) and (max-width: 1200px) and (orientation: landscape) {
  .controls {
    flex-wrap: wrap;
    align-items: flex-end;
    gap: 8px 10px;
    padding: 8px 10px;
    max-height: 174px;
    overflow-y: auto;
    scrollbar-width: none;
  }

  .controls::-webkit-scrollbar {
    display: none;
  }

  .group {
    flex: 0 1 132px;
    min-width: 112px;
  }

  .group.transition-group {
    flex: 1 1 360px;
    min-width: 300px;
  }

  .group.export-preset-group {
    flex: 0 1 210px;
    min-width: 190px;
  }

  .controls > button {
    flex: 0 0 auto;
  }

  .stage {
    grid-template-columns: minmax(0, 1fr) minmax(280px, 32vw);
    grid-template-rows: minmax(0, 1fr);
  }

  .stage.points-collapsed {
    grid-template-columns: minmax(0, 1fr) 32px;
    grid-template-rows: minmax(0, 1fr);
  }

  .points {
    border-left: 1px solid var(--rule);
    border-top: none;
    height: auto;
    min-height: 0;
    padding: 12px 12px 12px 18px;
  }

  .points.collapsed {
    height: auto;
    padding: 8px 0;
  }

  .points-toggle {
    top: 8px;
    left: 4px;
  }

  .points.collapsed .points-toggle {
    margin: 0 auto;
  }

  .dual-pane {
    grid-template-columns: 1fr 1fr;
    grid-template-rows: minmax(0, 1fr);
  }

  .pane {
    border-right: 1px solid var(--rule);
    border-bottom: none;
  }

  .pane:last-child {
    border-right: none;
  }

  .pane-meta {
    white-space: nowrap;
  }

  .modal {
    width: min(720px, calc(100vw - 32px));
  }
  .modal.modal-with-preview {
    width: min(960px, calc(100vw - 32px));
  }

  .modal-body {
    display: grid;
    grid-template-columns: 1fr 1fr;
  }

  .mrow,
  .mrow.source,
  .mrow.estimate,
  .local-mode-row {
    grid-column: auto;
  }

  .mrow.estimate,
  .mrow.color-mode-row,
  .local-mode-row,
  .progress-stack,
  .modal-status,
  .preview-grid,
  .local-path-list {
    grid-column: 1 / -1;
  }

  .preview-grid {
    grid-template-columns: repeat(2, minmax(0, 1fr));
  }
}
</style>
