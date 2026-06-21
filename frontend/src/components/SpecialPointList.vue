<script setup lang="ts">
import { computed, onBeforeUnmount, ref, watch } from 'vue'
import {
  api,
  type SpecialPointEnumResult,
  type SpecialPointEnumResponse,
  type SpecialPointKind,
  type SpecialPointSearchResponse,
  type SpecialPointViewport,
} from '../api'
import { lang } from '../i18n'

const props = defineProps<{
  viewport?: SpecialPointViewport
  hoveredId?: string
  selectedId?: string
  variantHint?: string
}>()

type PanelMode = 'search' | 'enumerate'
type SpecialPointPanelResponse = SpecialPointEnumResponse | SpecialPointSearchResponse

const LOCAL_CENTER_MAX_PERIOD = 16384
const LOCAL_CENTER_MAX_ATTEMPTS = 8192
const LOCAL_VIEWPORT_SEED_BUDGET = 160
const LOCAL_MISI_MAX_PREPERIOD = 4096
const LOCAL_MISI_MAX_PERIOD = 4096
const LOCAL_MISI_MAX_SUM = 8192
const LOCAL_MISI_MAX_PAIRS = 16384
const DEFAULT_LOCAL_CENTER_PERIOD_MAX = LOCAL_CENTER_MAX_ATTEMPTS
const DEFAULT_ENUM_CENTER_PERIOD_MAX = 8
const DEFAULT_MISI_PERIOD_MAX = 4

const emit = defineEmits<{
  (e: 'import-point', p: SpecialPointEnumResult): void
  (e: 'hover-point', id: string): void
  (e: 'select-point', p: SpecialPointEnumResult): void
  (e: 'results-updated', points: SpecialPointEnumResult[]): void
  (e: 'use-julia', p: SpecialPointEnumResult): void
}>()

const panelMode = ref<PanelMode>(props.viewport ? 'search' : 'enumerate')
const autoSearch = ref(!!props.viewport)
const kind = ref<SpecialPointKind>('center')
const periodMin = ref(1)
const periodMax = ref(props.viewport ? DEFAULT_LOCAL_CENTER_PERIOD_MAX : DEFAULT_ENUM_CENTER_PERIOD_MAX)
const preperiodMin = ref(1)
const preperiodMax = ref(3)
const includeVariantExistence = ref(true)
const includeRejectedDebug = ref(false)
const visibleOnly = ref(true)
const seedsPerBatch = ref(2048)
const maxSeedBatches = ref(80)
const running = ref(false)
const message = ref('')
const result = ref<SpecialPointPanelResponse | null>(null)
const variantFilter = ref('')
const currentRunId = ref('')
let searchSeq = 0
let searchTimer: ReturnType<typeof setTimeout> | null = null

watch(kind, (next) => {
  if (next === 'misiurewicz') {
    preperiodMin.value = 1
    preperiodMax.value = Math.min(preperiodMax.value, DEFAULT_MISI_PERIOD_MAX)
    periodMin.value = 1
    periodMax.value = Math.min(periodMax.value, DEFAULT_MISI_PERIOD_MAX)
  } else {
    periodMin.value = 1
    periodMax.value = panelMode.value === 'search'
      ? Math.max(periodMax.value, DEFAULT_LOCAL_CENTER_PERIOD_MAX)
      : Math.min(periodMax.value, DEFAULT_ENUM_CENTER_PERIOD_MAX)
  }
})

function expectedCenterCount(p: number): number {
  const counts: Record<number, number> = {}
  for (let n = 1; n <= p; n++) {
    let c = Math.pow(2, n - 1)
    for (let d = 1; d < n; d++) if (n % d === 0) c -= counts[d]
    counts[n] = c
  }
  return counts[p] ?? 0
}

function expectedMisiurewiczCount(m: number, p: number): number {
  if (m < 1 || p < 1 || m > 6 || p > 6 || m + p > 10) return -1
  if (m === 1) return 0
  let count = 2 * expectedCenterCount(p) * Math.pow(2, m - 2)
  if ((m - 1) % p === 0) count -= expectedCenterCount(p)
  return count
}

const expectedInfo = computed(() => {
  let total = 0
  if (kind.value === 'center') {
    if (periodMin.value < 1 || periodMax.value < periodMin.value || periodMax.value > 10) {
      return { ok: false, total: 0, text: lang.value === 'en' ? 'period range must be 1..10' : '周期范围须为 1..10' }
    }
    for (let p = periodMin.value; p <= periodMax.value; p++) total += expectedCenterCount(p)
  } else {
    if (preperiodMin.value < 1 || preperiodMax.value < preperiodMin.value || preperiodMax.value > 6 ||
        periodMin.value < 1 || periodMax.value < periodMin.value || periodMax.value > 6 ||
        preperiodMax.value + periodMax.value > 10) {
      return { ok: false, total: 0, text: lang.value === 'en' ? 'preperiod 1..6, period 1..6, sum <= 10' : '前周期 1..6，周期 1..6，总和 <= 10' }
    }
    for (let m = preperiodMin.value; m <= preperiodMax.value; m++) {
      for (let p = periodMin.value; p <= periodMax.value; p++) {
        const c = expectedMisiurewiczCount(m, p)
        if (c < 0) return { ok: false, total: 0, text: lang.value === 'en' ? `count unavailable for m=${m}, p=${p}` : `m=${m}, p=${p} 的数量不可用` }
        total += c
      }
    }
  }
  if (total > 3000) return { ok: false, total, text: lang.value === 'en' ? 'expected count exceeds 3000' : '预期数量超过 3000' }
  return { ok: true, total, text: lang.value === 'en' ? `${total} expected` : `预期 ${total} 个` }
})

const searchInfo = computed(() => {
  if (!props.viewport) return { ok: false, text: lang.value === 'en' ? 'viewport required' : '需要视口' }
  const maxPeriod = kind.value === 'misiurewicz' ? LOCAL_MISI_MAX_PERIOD : LOCAL_CENTER_MAX_PERIOD
  if (periodMin.value < 1 || periodMax.value < periodMin.value || periodMax.value > maxPeriod) {
    return { ok: false, text: lang.value === 'en' ? `period range must be 1..${maxPeriod}` : `周期范围须为 1..${maxPeriod}` }
  }
  if (kind.value === 'center') {
    const attempts = periodMax.value - periodMin.value + 1
    if (attempts > LOCAL_CENTER_MAX_ATTEMPTS) return { ok: false, text: lang.value === 'en' ? `local center search exceeds ${LOCAL_CENTER_MAX_ATTEMPTS} periods` : `局部中心搜索超过 ${LOCAL_CENTER_MAX_ATTEMPTS} 个周期` }
  }
  if (kind.value === 'misiurewicz' &&
      (preperiodMin.value < 1 || preperiodMax.value < preperiodMin.value || preperiodMax.value > LOCAL_MISI_MAX_PREPERIOD ||
       preperiodMax.value + periodMax.value > LOCAL_MISI_MAX_SUM)) {
    return { ok: false, text: lang.value === 'en' ? `preperiod 1..${LOCAL_MISI_MAX_PREPERIOD}, period 1..${LOCAL_MISI_MAX_PERIOD}, sum <= ${LOCAL_MISI_MAX_SUM}` : `前周期 1..${LOCAL_MISI_MAX_PREPERIOD}，周期 1..${LOCAL_MISI_MAX_PERIOD}，总和 <= ${LOCAL_MISI_MAX_SUM}` }
  }
  if (kind.value === 'misiurewicz') {
    const tasks = (preperiodMax.value - preperiodMin.value + 1) * (periodMax.value - periodMin.value + 1)
    if (tasks > LOCAL_MISI_MAX_PAIRS) return { ok: false, text: lang.value === 'en' ? `local Misiurewicz search exceeds ${LOCAL_MISI_MAX_PAIRS} pairs` : `局部 Misiurewicz 搜索超过 ${LOCAL_MISI_MAX_PAIRS} 对` }
  }
  const attempts = kind.value === 'misiurewicz'
    ? (preperiodMax.value - preperiodMin.value + 1) * (periodMax.value - periodMin.value + 1)
    : (periodMax.value - periodMin.value + 1)
  return {
    ok: true,
    text: kind.value === 'misiurewicz'
      ? (lang.value === 'en' ? `center Newton · ${attempts} pairs · first match` : `中心牛顿法 · ${attempts} 对 · 首个匹配`)
      : (lang.value === 'en' ? `viewport Newton · ${attempts} periods · up to ${LOCAL_VIEWPORT_SEED_BUDGET} view samples` : `视口牛顿法 · ${attempts} 个周期 · 最多 ${LOCAL_VIEWPORT_SEED_BUDGET} 个视图采样`),
  }
})
const activeInfo = computed(() => panelMode.value === 'search' ? searchInfo.value : expectedInfo.value)
const workflowHelpTitle = computed(() => panelMode.value === 'search'
  ? (lang.value === 'en' ? 'How local solve scans' : '局部求解如何扫描')
  : (lang.value === 'en' ? 'How full enumerate works' : '完整枚举如何工作'))
const workflowHelp = computed(() => {
  if (panelMode.value !== 'search') {
    return lang.value === 'en'
      ? [
        'Full enumerate samples the global radius-2 disk and tries to find every expected root in the small requested range.',
        'It is exhaustive only when complete=true and acceptedCount equals expectedCount.',
      ]
      : [
        '完整枚举对全局半径为 2 的圆盘采样，并尝试在请求的小范围内找出每一个预期根。',
        '仅当 complete=true 且 acceptedCount 等于 expectedCount 时才是穷尽的。',
      ]
  }
  if (kind.value === 'misiurewicz') {
    return lang.value === 'en'
      ? [
        'Local solve uses the current map center as the only Newton initial value; it does not randomly scan the viewport.',
        'Order: preperiod asc, then period asc. Example m=2..4, p=1..3 tries (2,1), (2,2), (2,3), (3,1)...',
        'Unconverged tasks are retried with higher Newton iteration limits before moving on.',
        `It stops at the first exact Misiurewicz match. If none match, it shows the classified candidate with the largest actual period. Limits: m/p <= ${LOCAL_MISI_MAX_PERIOD}, m+p <= ${LOCAL_MISI_MAX_SUM}, selected pairs <= ${LOCAL_MISI_MAX_PAIRS}.`,
      ]
      : [
        '局部求解仅以当前映射中心作为唯一的牛顿初值；它不会随机扫描视口。',
        '顺序：先按前周期升序，再按周期升序。例如 m=2..4, p=1..3 会尝试 (2,1)、(2,2)、(2,3)、(3,1)……',
        '未收敛的任务会以更高的牛顿迭代上限重试，然后再继续。',
        `在首个精确的 Misiurewicz 匹配处停止。若无匹配，则显示实际周期最大的已分类候选。限制：m/p <= ${LOCAL_MISI_MAX_PERIOD}，m+p <= ${LOCAL_MISI_MAX_SUM}，所选对数 <= ${LOCAL_MISI_MAX_PAIRS}。`,
      ]
  }
  return lang.value === 'en'
    ? [
      'Viewport solve evaluates center equations in ascending period waves; the first wave with visible matches stops the search.',
      'All matches already computed in that stopping wave are returned and marked on the viewport.',
      'Within each wave, deterministic viewport samples are only tried for the same hinted period, so higher-period hints never skip lower-period waves.',
      'Unconverged tasks are retried with higher Newton iteration limits before moving on.',
      `It stops at the first matching wave. If none match, it shows the classified candidate with the largest actual period. Limits: periodMax <= ${LOCAL_CENTER_MAX_PERIOD}, selected span <= ${LOCAL_CENTER_MAX_ATTEMPTS} periods, viewport samples <= ${LOCAL_VIEWPORT_SEED_BUDGET}.`,
    ]
    : [
      '视口求解按周期升序的波次求解中心方程；第一个含可见匹配的波次会停止搜索。',
      '停止波次中已计算出的所有匹配都会返回并标记在视口上。',
      '在每个波次内，确定性视口采样仅针对相同提示周期尝试，因此高周期提示绝不会跳过低周期波次。',
      '未收敛的任务会以更高的牛顿迭代上限重试，然后再继续。',
      `在首个匹配波次处停止。若无匹配，则显示实际周期最大的已分类候选。限制：periodMax <= ${LOCAL_CENTER_MAX_PERIOD}，所选跨度 <= ${LOCAL_CENTER_MAX_ATTEMPTS} 个周期，视口采样 <= ${LOCAL_VIEWPORT_SEED_BUDGET}。`,
    ]
})
const periodInputMax = computed(() => {
  if (panelMode.value === 'search') return kind.value === 'misiurewicz' ? LOCAL_MISI_MAX_PERIOD : LOCAL_CENTER_MAX_PERIOD
  return kind.value === 'misiurewicz' ? 6 : 10
})
const preperiodInputMax = computed(() => panelMode.value === 'search' ? LOCAL_MISI_MAX_PREPERIOD : 6)
const points = computed(() => result.value?.points ?? [])
const visiblePoints = computed(() => {
  if (!variantFilter.value) return points.value
  return points.value.filter(p => {
    if (p.compatibleVariants?.includes(variantFilter.value)) return true
    return p.variants?.some(v => v.variant_name === variantFilter.value && v.exists)
  })
})
const variants = computed(() => {
  const names = new Set<string>()
  for (const p of points.value) {
    for (const v of p.compatibleVariants || []) names.add(v)
    for (const v of p.variants || []) if (v.exists) names.add(v.variant_name)
  }
  return [...names]
})
const grouped = computed(() => {
  const groups = new Map<string, SpecialPointEnumResult[]>()
  for (const p of visiblePoints.value) {
    const key = p.kind === 'center' ? `period ${p.period}` : `m${p.preperiod} p${p.period}`
    if (!groups.has(key)) groups.set(key, [])
    groups.get(key)!.push(p)
  }
  return [...groups.entries()]
})
const enumIncomplete = computed(() => !!result.value && 'complete' in result.value && !result.value.complete)
const enumIncompleteText = computed(() => {
  if (!result.value || !('expectedCount' in result.value) || !enumIncomplete.value) return ''
  return lang.value === 'en'
    ? `incomplete: ${result.value.acceptedCount}/${result.value.expectedCount} roots`
    : `不完整：${result.value.acceptedCount}/${result.value.expectedCount} 个根`
})
const searchNoPoint = computed(() =>
  panelMode.value === 'search' &&
  !!result.value &&
  'noPoint' in result.value &&
  !!result.value.noPoint
)
const emptyText = computed(() => {
  if (running.value) {
    if (panelMode.value !== 'search') return lang.value === 'en' ? 'Enumerating roots...' : '正在枚举根……'
    return kind.value === 'misiurewicz'
      ? (lang.value === 'en' ? 'Solving from current center...' : '正在从当前中心求解……')
      : (lang.value === 'en' ? 'Sampling current viewport...' : '正在采样当前视口……')
  }
  if (searchNoPoint.value) {
    return kind.value === 'misiurewicz'
      ? (lang.value === 'en' ? 'No matching local Misiurewicz point found from the current center.' : '未从当前中心找到匹配的局部 Misiurewicz 点。')
      : (lang.value === 'en' ? 'No matching local hyperbolic center found in the current viewport.' : '未在当前视口找到匹配的局部双曲中心。')
  }
  return panelMode.value === 'search'
    ? (lang.value === 'en' ? 'No local solve results yet.' : '暂无局部求解结果。')
    : (lang.value === 'en' ? 'No enumeration results yet.' : '暂无枚举结果。')
})

function isFallbackPoint(p: SpecialPointEnumResult) {
  return !!p.fallback || !p.accepted
}

function actualPointLabel(p: SpecialPointEnumResult) {
  const actual = p.actual
  if (!actual?.found_repeat) return lang.value === 'en' ? 'no repeat' : '无重复'
  if (actual.is_center) return `center p${actual.period}`
  if (actual.is_misiurewicz) return `m${actual.preperiod} p${actual.period}`
  return `period ${actual.period}`
}

function delay(ms: number) {
  return new Promise(resolve => setTimeout(resolve, ms))
}

function cancelCurrentSearch() {
  if (!currentRunId.value) return
  api.cancelRun(currentRunId.value).catch(() => {})
  currentRunId.value = ''
}

function cancelRunningSearch() {
  searchSeq += 1
  if (searchTimer) clearTimeout(searchTimer)
  cancelCurrentSearch()
  running.value = false
  message.value = lang.value === 'en' ? 'cancelled' : '已取消'
}

async function searchViewport(manual = false) {
  if (!searchInfo.value.ok || !props.viewport) return
  const seq = ++searchSeq
  cancelCurrentSearch()
  running.value = true
  message.value = kind.value === 'misiurewicz'
    ? (lang.value === 'en' ? 'solving from current center...' : '正在从当前中心求解……')
    : (lang.value === 'en' ? 'sampling current viewport...' : '正在采样当前视口……')
  result.value = null
  emit('results-updated', [])
  try {
    const started = await api.specialPointsSearch({
      kind: kind.value,
      periodMin: periodMin.value,
      periodMax: periodMax.value,
      preperiodMin: preperiodMin.value,
      preperiodMax: preperiodMax.value,
      seedBudget: kind.value === 'center' ? LOCAL_VIEWPORT_SEED_BUDGET : undefined,
      includeVariantCompatibility: includeVariantExistence.value,
      visibleOnly: visibleOnly.value,
      viewport: props.viewport,
    })
    if (seq !== searchSeq) return
    currentRunId.value = started.runId
    message.value = lang.value === 'en' ? `${started.runId} · searching...` : `${started.runId} · 正在搜索……`

    for (;;) {
      await delay(260)
      if (seq !== searchSeq) return
      const resp = await api.specialPointsResults(started.runId) as SpecialPointSearchResponse
      if (seq !== searchSeq) return
      if (resp.status === 'running' || resp.status === 'queued') {
        message.value = lang.value === 'en'
          ? `solving... ${resp.acceptedCount || 0} found · ${resp.seedCount || 0} attempts`
          : `正在求解…… 已找到 ${resp.acceptedCount || 0} 个 · ${resp.seedCount || 0} 次尝试`
        continue
      }
      if (resp.status === 'cancelled') {
        message.value = lang.value === 'en' ? 'cancelled' : '已取消'
        return
      }
      if (resp.status === 'failed') {
        message.value = lang.value === 'en' ? 'failed' : '失败'
        return
      }
      result.value = resp
      emit('results-updated', resp.points)
      const label = lang.value === 'en'
        ? (kind.value === 'misiurewicz' ? 'local Misiurewicz point' : 'viewport hyperbolic center')
        : (kind.value === 'misiurewicz' ? '局部 Misiurewicz 点' : '视口双曲中心')
      const fallback = resp.points.find(p => isFallbackPoint(p))
      if (lang.value === 'en') {
        const solve = manual ? 'solve' : 'auto solve'
        message.value = fallback
          ? `${solve}: no exact ${label}; showing fallback ${actualPointLabel(fallback)} · ${resp.seedCount} attempts`
          : resp.noPoint
          ? `${solve}: no ${label} found · ${resp.seedCount} attempts`
          : `${solve}: ${resp.acceptedCount} ${label} · ${resp.seedCount} attempts`
      } else {
        const solve = manual ? '求解' : '自动求解'
        message.value = fallback
          ? `${solve}：无精确 ${label}；显示回退 ${actualPointLabel(fallback)} · ${resp.seedCount} 次尝试`
          : resp.noPoint
          ? `${solve}：未找到 ${label} · ${resp.seedCount} 次尝试`
          : `${solve}：${resp.acceptedCount} 个 ${label} · ${resp.seedCount} 次尝试`
      }
      return
    }
  } catch (e: any) {
    if (seq === searchSeq) message.value = (lang.value === 'en' ? 'failed: ' : '失败：') + (e?.data?.error || e?.message || e)
  } finally {
    if (seq === searchSeq) {
      running.value = false
      currentRunId.value = ''
    }
  }
}

async function enumerate() {
  if (!expectedInfo.value.ok) return
  running.value = true
  message.value = lang.value === 'en' ? 'enumerating...' : '正在枚举……'
  result.value = null
  emit('results-updated', [])
  try {
    const resp = await api.specialPointsEnumerate({
      kind: kind.value,
      periodMin: periodMin.value,
      periodMax: periodMax.value,
      preperiodMin: preperiodMin.value,
      preperiodMax: preperiodMax.value,
      includeVariantExistence: includeVariantExistence.value,
      includeRejectedDebug: includeRejectedDebug.value,
      visibleOnly: visibleOnly.value,
      seedsPerBatch: seedsPerBatch.value,
      maxSeedBatches: maxSeedBatches.value,
      viewport: props.viewport,
    })
    result.value = resp
    emit('results-updated', resp.points)
    message.value = lang.value === 'en'
      ? `${resp.status}: ${resp.acceptedCount}/${resp.expectedCount} roots, ${resp.seedCount} seeds`
      : `${resp.status}：${resp.acceptedCount}/${resp.expectedCount} 个根，${resp.seedCount} 个种子`
  } catch (e: any) {
    message.value = (lang.value === 'en' ? 'failed: ' : '失败：') + (e?.data?.error || e?.message || e)
  } finally {
    running.value = false
  }
}

function runActive() {
  if (panelMode.value === 'search') searchViewport(true)
  else enumerate()
}

function copyPoint(p: SpecialPointEnumResult) {
  navigator.clipboard?.writeText(`${p.re}, ${p.im}`)
}

function addBookmark(p: SpecialPointEnumResult) {
  const raw = localStorage.getItem('fs_special_point_bookmarks')
  const items = raw ? JSON.parse(raw) : []
  items.push({ id: p.id, re: p.re, im: p.im, kind: p.kind, period: p.period, preperiod: p.preperiod, createdAt: new Date().toISOString() })
  localStorage.setItem('fs_special_point_bookmarks', JSON.stringify(items.slice(-500)))
}

function selectPoint(p: SpecialPointEnumResult) {
  emit('select-point', p)
  emit('import-point', p)
}

watch(panelMode, (mode) => {
  if (mode === 'search') {
    visibleOnly.value = true
    if (kind.value === 'center') periodMax.value = Math.max(periodMax.value, DEFAULT_LOCAL_CENTER_PERIOD_MAX)
  } else if (kind.value === 'center') {
    periodMax.value = Math.min(periodMax.value, DEFAULT_ENUM_CENTER_PERIOD_MAX)
  }
})

watch(
  () => ({
    mode: panelMode.value,
    auto: autoSearch.value,
    viewport: props.viewport ? `${props.viewport.centerRe}:${props.viewport.centerIm}:${props.viewport.scale}:${props.viewport.width}:${props.viewport.height}` : '',
    periodMin: periodMin.value,
    periodMax: periodMax.value,
    preperiodMin: preperiodMin.value,
    preperiodMax: preperiodMax.value,
    kind: kind.value,
    variants: includeVariantExistence.value,
    visibleOnly: visibleOnly.value,
  }),
  () => {
    if (panelMode.value !== 'search' || !autoSearch.value || !props.viewport) return
    if (searchTimer) clearTimeout(searchTimer)
    searchTimer = setTimeout(() => searchViewport(false), 450)
  },
  { immediate: true }
)

onBeforeUnmount(() => {
  searchSeq += 1
  if (searchTimer) clearTimeout(searchTimer)
  cancelCurrentSearch()
})

defineExpose({ enumerate, refresh: runActive, points })
</script>

<template>
  <div class="sp-panel">
    <div class="head">
      <span class="panel-title">{{ lang === 'en' ? 'Special Points' : '特殊点' }}</span>
      <button class="primary" @click="runActive" :disabled="running || !activeInfo.ok">
        {{ running ? (lang === 'en' ? 'running' : '运行中') : (panelMode === 'search' ? (lang === 'en' ? 'search' : '搜索') : (lang === 'en' ? 'enumerate' : '枚举')) }}
      </button>
      <button v-if="running && panelMode === 'search'" @click="cancelRunningSearch">{{ lang === 'en' ? 'cancel' : '取消' }}</button>
    </div>

    <div class="controls-grid">
      <label>{{ lang === 'en' ? 'workflow' : '工作流' }}</label>
      <select v-model="panelMode">
        <option value="search">{{ lang === 'en' ? 'Viewport local solve' : '视口局部求解' }}</option>
        <option value="enumerate">{{ lang === 'en' ? 'Full enumerate' : '完整枚举' }}</option>
      </select>
      <label>{{ lang === 'en' ? 'mode' : '模式' }}</label>
      <select v-model="kind">
        <option value="center">{{ lang === 'en' ? 'Hyperbolic centers' : '双曲中心' }}</option>
        <option value="misiurewicz">{{ lang === 'en' ? 'Misiurewicz points' : 'Misiurewicz 点' }}</option>
      </select>
      <label v-if="kind === 'misiurewicz'">{{ lang === 'en' ? 'preperiod' : '前周期' }}</label>
      <div v-if="kind === 'misiurewicz'" class="pair">
        <input type="number" v-model.number="preperiodMin" min="1" :max="preperiodInputMax" />
        <input type="number" v-model.number="preperiodMax" min="1" :max="preperiodInputMax" />
      </div>
      <label>{{ lang === 'en' ? 'period' : '周期' }}</label>
      <div class="pair">
        <input type="number" v-model.number="periodMin" min="1" :max="periodInputMax" />
        <input type="number" v-model.number="periodMax" min="1" :max="periodInputMax" />
      </div>
      <label v-if="panelMode === 'enumerate'">{{ lang === 'en' ? 'seeds' : '种子' }}</label>
      <div v-if="panelMode === 'enumerate'" class="pair">
        <input type="number" v-model.number="seedsPerBatch" min="1" max="10000" />
        <input type="number" v-model.number="maxSeedBatches" min="1" max="200" />
      </div>
    </div>

    <div class="opts">
      <label v-if="panelMode === 'search'"><input type="checkbox" v-model="autoSearch" /> {{ lang === 'en' ? 'auto' : '自动' }}</label>
      <label><input type="checkbox" v-model="includeVariantExistence" /> {{ lang === 'en' ? 'variants' : '变体' }}</label>
      <label><input type="checkbox" v-model="visibleOnly" /> {{ lang === 'en' ? 'visible only' : '仅可见' }}</label>
      <label v-if="panelMode === 'enumerate'"><input type="checkbox" v-model="includeRejectedDebug" /> {{ lang === 'en' ? 'rejected' : '已拒绝' }}</label>
    </div>

    <div class="status mono" :class="{ bad: !activeInfo.ok }">{{ activeInfo.text }}</div>
    <details class="workflow-help">
      <summary>{{ workflowHelpTitle }}</summary>
      <div v-for="line in workflowHelp" :key="line" class="help-line">{{ line }}</div>
    </details>
    <div v-if="message" class="status mono">{{ message }}</div>
    <div v-if="props.variantHint" class="status hint mono">{{ props.variantHint }}</div>
    <div v-if="enumIncompleteText" class="status warn mono">{{ enumIncompleteText }}</div>
    <div v-if="panelMode === 'search' && result?.warning" class="status warn mono">{{ result.warning }}</div>

    <div v-if="variants.length" class="filters">
      <button :class="{ active: !variantFilter }" @click="variantFilter = ''">{{ lang === 'en' ? 'all' : '全部' }}</button>
      <button v-for="v in variants" :key="v" :class="{ active: variantFilter === v }" @click="variantFilter = v">{{ v }}</button>
    </div>

    <div v-if="grouped.length" class="groups">
      <section v-for="[name, pts] in grouped" :key="name" class="group-block">
        <div class="group-title mono">{{ name }} · {{ pts.length }}</div>
        <div
          v-for="p in pts"
          :key="p.id"
          class="point-row"
          :class="{ hover: hoveredId === p.id, selected: selectedId === p.id, fallback: isFallbackPoint(p) }"
          @mouseenter="$emit('hover-point', p.id)"
          @mouseleave="$emit('hover-point', '')"
          @click="selectPoint(p)">
          <div class="coord mono">
            <span>{{ p.re.toFixed(10) }}</span>
            <span>{{ p.im.toFixed(10) }}</span>
          </div>
          <div class="meta mono">
            <span v-if="isFallbackPoint(p)" class="fallback-label">{{ lang === 'en' ? 'fallback' : '回退' }} · {{ actualPointLabel(p) }} · </span>
            {{ lang === 'en' ? 'res' : '残差' }} {{ p.residual.toExponential(1) }} · {{ p.newtonIterations }} {{ lang === 'en' ? 'it' : '次迭代' }}
          </div>
          <div v-if="(p.compatibleVariants?.length || p.variants?.length)" class="tags">
            <button
              v-for="v in (p.compatibleVariants?.length ? p.compatibleVariants : p.variants.filter(v => v.exists).map(v => v.variant_name))"
              :key="v"
              class="tag"
              @click.stop="variantFilter = v">
              {{ v }}
            </button>
          </div>
          <div class="actions">
            <button @click.stop="copyPoint(p)">{{ lang === 'en' ? 'copy' : '复制' }}</button>
            <button @click.stop="$emit('use-julia', p)">{{ lang === 'en' ? 'Julia c' : 'Julia c' }}</button>
            <button @click.stop="addBookmark(p)">{{ lang === 'en' ? 'bookmark' : '书签' }}</button>
          </div>
        </div>
      </section>
    </div>
    <div v-else class="empty mono" :class="{ warn: searchNoPoint }">{{ emptyText }}</div>
  </div>
</template>

<style scoped>
.sp-panel { display: flex; flex-direction: column; gap: 10px; }
.head { display: flex; align-items: center; justify-content: space-between; gap: 8px; }
.controls-grid {
  display: grid;
  grid-template-columns: 72px 1fr;
  align-items: center;
  gap: 6px 8px;
}
.controls-grid label,
.opts label {
  color: var(--text-dim);
  font-size: var(--fs-label);
  text-transform: uppercase;
  letter-spacing: 0.06em;
}
.pair { display: grid; grid-template-columns: 1fr 1fr; gap: 6px; }
.opts { display: flex; flex-wrap: wrap; gap: 8px; }
.opts input { width: auto; margin-right: 4px; }
.status { color: var(--text-dim); font-size: 10px; }
.status.bad { color: var(--bad); }
.status.hint { color: var(--accent); }
.status.warn { color: #d5ad45; }
.workflow-help {
  color: var(--text-faint);
  font-size: 10px;
  line-height: 1.35;
}
.workflow-help summary {
  cursor: pointer;
  color: var(--text-dim);
  user-select: none;
}
.workflow-help[open] summary { margin-bottom: 4px; }
.help-line + .help-line { margin-top: 3px; }
.filters { display: flex; flex-wrap: wrap; gap: 5px; }
.filters button,
.tag {
  padding: 2px 6px;
  font-size: 9px;
}
.filters button.active { border-color: var(--accent); color: var(--accent); }
.groups { display: flex; flex-direction: column; gap: 10px; }
.group-title {
  color: var(--accent);
  font-size: 10px;
  text-transform: uppercase;
  letter-spacing: 0.08em;
  margin-bottom: 5px;
}
.point-row {
  border: 1px solid var(--rule);
  padding: 7px;
  margin-bottom: 5px;
  cursor: pointer;
  background: rgba(255,255,255,0.015);
}
.point-row.hover,
.point-row:hover { border-color: var(--accent); background: var(--accent-weak); }
.point-row.selected { border-color: var(--accent); box-shadow: inset 2px 0 0 var(--accent); }
.point-row.fallback { border-style: dashed; border-color: #d5ad45; }
.coord {
  display: grid;
  grid-template-columns: 1fr;
  gap: 2px;
  font-size: 10px;
  color: var(--text);
}
.meta { color: var(--text-faint); font-size: 9px; margin-top: 4px; }
.fallback-label { color: #d5ad45; }
.tags { display: flex; flex-wrap: wrap; gap: 4px; margin-top: 5px; }
.actions { display: flex; gap: 5px; margin-top: 6px; }
.actions button { padding: 2px 5px; font-size: 9px; }
.empty { color: var(--text-faint); font-size: 10px; padding: 8px 0; }
.empty.warn { color: #d5ad45; }
.num { font-variant-numeric: tabular-nums; }

@media (max-width: 760px), ((pointer: coarse) and (max-width: 1200px)), ((any-pointer: coarse) and (max-width: 1200px)), ((min-width: 761px) and (max-width: 1200px) and (orientation: landscape)) {
  .head {
    flex-wrap: wrap;
    align-items: stretch;
  }

  .head .panel-title {
    flex: 1 1 100%;
  }

  .head button {
    flex: 1 1 104px;
  }

  .controls-grid {
    grid-template-columns: 1fr;
    gap: 5px;
  }

  .controls-grid label {
    margin-bottom: 0;
  }

  .opts {
    gap: 6px;
  }

  .opts label {
    flex: 1 1 110px;
  }

  .coord {
    font-size: 9px;
    overflow-wrap: anywhere;
  }

  .actions {
    flex-wrap: wrap;
  }

  .actions button {
    flex: 1 1 92px;
    min-height: 30px;
  }
}
</style>
