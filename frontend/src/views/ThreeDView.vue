<script setup lang="ts">
import { ref, computed, watch } from 'vue'
import ThreeDViewer from '../components/ThreeDViewer.vue'
import { api, VARIANTS, VARIANT_LABELS, type Variant, type HsStage, type TransitionVoxelResponse, type HsFieldResponse, type MeshResponse } from '../api'
import { t, lang } from '../i18n'
import { promptSlowRenderWarning, slowRenderWarningsDisabled } from '../slowWarnings'

type Mode = 'hs' | 'transition'

const mode = ref<Mode>('hs')

// HS mode params
const hsMetric  = ref<HsStage>('min_abs')
const hsVariant = ref<Variant>('mandelbrot')
const hsRes     = ref(256)
const hsIter    = ref(512)
const hsPairwiseCap = ref(64)
const hsCenterRe = ref(-0.75)
const hsCenterIm = ref(0.0)
const hsScale    = ref(3.0)

// HS z-scale: sign (convex=+1 / concave=-1) + log exponent
const hsZSign = ref<1 | -1>(1)           // 1 = convex (height up), -1 = concave (inverted)
const hsZExp  = ref(-1.0)                 // log10 magnitude: range -15..0 → 10^exp
const hsZScale = computed(() => hsZSign.value * Math.pow(10, hsZExp.value))

// Transition mode params — voxel-only
const txRes      = ref(64)
const txIso      = ref(0.48)
const txIter     = ref(128)
const txCenterX  = ref(0.0)
const txCenterY  = ref(0.0)
const txCenterZ  = ref(0.0)
const txExtent   = ref(2.0)
const txFrom     = ref<string>('mandelbrot')
const txTo       = ref<string>('burning_ship')

// State
const hsFieldData  = ref<HsFieldResponse | null>(null)
const voxelData    = ref<TransitionVoxelResponse | null>(null)
const stlUrl       = ref<string | null>(null)
const loading      = ref(false)
const stlLoading   = ref(false)
const info         = ref('')
const error        = ref('')
let slowHsWarnKey = ''

const HS_METRICS: HsStage[] = ['min_abs', 'max_abs', 'envelope', 'min_pairwise_dist']
const AXIS_TRANSITION_VARIANTS = VARIANTS.slice(0, 10)

const HS_METRIC_LABELS: Record<HsStage, { en: string; zh: string }> = {
  min_abs:            { en: 'Min abs(z) (HS-base)',       zh: '最小 abs(z)（HS 基础）' },
  max_abs:            { en: 'Max abs(z) (envelope hi)',   zh: '最大 abs(z)（包络高）' },
  envelope:           { en: 'Envelope',                   zh: '包络' },
  min_pairwise_dist:  { en: 'Min pairwise (recurrence)',  zh: '最小轨道距（递归）' },
}

function transitionStlDownloadUrl(r: TransitionVoxelResponse): string | null {
  if (r.stlArtifactId) return api.artifactDownloadUrl(r.stlArtifactId)
  if (!r.stlUrl) return null
  return new URL(r.stlUrl, api.baseUrl).toString()
}

function maybeWarnSlowHs(generatedMs: number, kind: string) {
  if (!(generatedMs > 1000)) return
  if (slowRenderWarningsDisabled()) return
  const key = [
    kind,
    hsMetric.value,
    hsVariant.value,
    hsRes.value,
    hsIter.value,
    hsPairwiseCap.value,
  ].join(':')
  if (slowHsWarnKey === key) return
  slowHsWarnKey = key
  promptSlowRenderWarning(`HS ${kind} took ${(generatedMs / 1000).toFixed(2)}s. For heavier recurrence detail this is expected; lower resolution/iterations/pairwise cap for faster interactive passes.`)
}

// ── HS field auto-compute (debounced) ─────────────────────────────────────────
let debounceTimer: ReturnType<typeof setTimeout> | null = null

function scheduleHsCompute() {
  if (mode.value !== 'hs') return
  if (debounceTimer) clearTimeout(debounceTimer)
  debounceTimer = setTimeout(computeHsField, 500)
}

watch([hsMetric, hsVariant, hsRes, hsIter, hsPairwiseCap, hsCenterRe, hsCenterIm, hsScale], scheduleHsCompute)

// ── HS field fetch ────────────────────────────────────────────────────────────

async function computeHsField() {
  loading.value = true
  error.value   = ''
  info.value    = lang.value === 'en' ? 'computing HS field…' : '正在计算 HS 场…'
  hsFieldData.value = null
  stlUrl.value  = null
  try {
    const r = await api.hsField({
      centerRe:   hsCenterRe.value,
      centerIm:   hsCenterIm.value,
      scale:      hsScale.value,
      resolution: hsRes.value,
      metric:     hsMetric.value,
      variant:    hsVariant.value,
      iterations: hsIter.value,
      pairwiseCap: hsPairwiseCap.value,
    })
    hsFieldData.value = r
    info.value = `${r.width}×${r.height} field · range [${r.fieldMin.toFixed(3)}, ${r.fieldMax.toFixed(3)}] · ${r.generatedMs.toFixed(0)}ms`
    maybeWarnSlowHs(r.generatedMs, 'field')
  } catch (e: any) {
    error.value = e?.data?.error ?? e?.message ?? String(e)
    info.value  = ''
  } finally {
    loading.value = false
  }
}

// ── HS STL export ─────────────────────────────────────────────────────────────

async function exportHsStl() {
  stlLoading.value = true
  error.value = ''
  stlUrl.value = null
  try {
    const r: MeshResponse = await api.hsMesh({
      centerRe:    hsCenterRe.value,
      centerIm:    hsCenterIm.value,
      scale:       hsScale.value,
      resolution:  hsRes.value,
      metric:      hsMetric.value,
      variant:     hsVariant.value,
      iterations:  hsIter.value,
      heightScale: Math.abs(hsZScale.value),
      pairwiseCap: hsPairwiseCap.value,
    })
    stlUrl.value = api.artifactDownloadUrl(r.stlArtifactId)
    if (r.generatedMs !== undefined) maybeWarnSlowHs(r.generatedMs, 'mesh')
  } catch (e: any) {
    error.value = e?.data?.error ?? e?.message ?? String(e)
  } finally {
    stlLoading.value = false
  }
}

// ── Transition voxel compute ──────────────────────────────────────────────────

async function computeTransitionVoxels() {
  loading.value   = true
  error.value     = ''
  info.value      = lang.value === 'en' ? 'computing voxel field…' : '正在计算体素场…'
  voxelData.value = null
  stlUrl.value    = null
  try {
    const r = await api.transitionVoxels({
      centerX:    txCenterX.value,
      centerY:    txCenterY.value,
      centerZ:    txCenterZ.value,
      extent:     txExtent.value,
      resolution: txRes.value,
      iso:        txIso.value,
      iterations: txIter.value,
      transitionFrom: txFrom.value,
      transitionTo:   txTo.value,
    })
    voxelData.value = r
    stlUrl.value = transitionStlDownloadUrl(r)
    info.value = `${(r.voxelCount ?? 0).toLocaleString()} voxels · ${r.faceCount.toLocaleString()} faces · ${r.resolution}³ grid · ${r.generatedMs.toFixed(0)}ms`
  } catch (e: any) {
    error.value = e?.data?.error ?? e?.message ?? String(e)
    info.value  = ''
  } finally {
    loading.value = false
  }
}

// ── Transition STL export (voxel faces → STL) ────────────────────────────────
// The voxels endpoint already generates the STL on the backend side.
// If a voxel render already happened, its stlUrl is available immediately.
// Otherwise we trigger a fresh voxel compute to get the STL.

async function exportTxStl() {
  if (voxelData.value?.stlArtifactId || voxelData.value?.stlUrl) {
    stlUrl.value = transitionStlDownloadUrl(voxelData.value)
    return
  }
  stlLoading.value = true
  error.value  = ''
  stlUrl.value = null
  try {
    const r = await api.transitionVoxels({
      centerX:    txCenterX.value,
      centerY:    txCenterY.value,
      centerZ:    txCenterZ.value,
      extent:     txExtent.value,
      resolution: txRes.value,
      iso:        txIso.value,
      iterations: txIter.value,
      transitionFrom: txFrom.value,
      transitionTo:   txTo.value,
    })
    voxelData.value = r
    stlUrl.value = transitionStlDownloadUrl(r)
    info.value = `${(r.voxelCount ?? 0).toLocaleString()} voxels · ${r.faceCount.toLocaleString()} faces · ${r.resolution}³ grid · ${r.generatedMs.toFixed(0)}ms`
  } catch (e: any) {
    error.value = e?.message ?? String(e)
  } finally {
    stlLoading.value = false
  }
}

function compute() {
  if (mode.value === 'hs') {
    computeHsField()
  } else {
    computeTransitionVoxels()
  }
}

// When switching to HS mode, auto-compute if no field yet
watch(mode, (m) => {
  if (m === 'hs' && !hsFieldData.value && !loading.value) {
    computeHsField()
  }
})

// ── HS pan/zoom from ThreeDViewer ─────────────────────────────────────────────
// ndx/ndy are normalized screen deltas (fraction of canvas width/height).
// Drag right (ndx > 0) → content follows finger → center moves right → Re decreases.
function onHsPan(ndx: number, ndy: number) {
  hsCenterRe.value -= ndx * hsScale.value
  hsCenterIm.value += ndy * hsScale.value  // screen Y down = Im up
}

function onHsZoom(factor: number) {
  hsScale.value = Math.max(1e-10, hsScale.value * factor)
}
</script>

<template>
  <div class="three-view">
    <!-- Left control strip -->
    <aside class="controls">
      <!-- Mode toggle -->
      <div class="mode-row">
        <button :class="['mode-btn', mode === 'hs' ? 'active' : '']" @click="mode = 'hs'">{{ t('three_mode_hs') }}</button>
        <button :class="['mode-btn', mode === 'transition' ? 'active' : '']" @click="mode = 'transition'">{{ t('three_mode_tx') }}</button>
      </div>

      <!-- HS params -->
      <template v-if="mode === 'hs'">
        <div class="group">
          <label>{{ t('three_metric') }}</label>
          <select v-model="hsMetric">
            <option v-for="m in HS_METRICS" :key="m" :value="m">{{ HS_METRIC_LABELS[m][lang] }}</option>
          </select>
        </div>
        <div class="group">
          <label>{{ t('variant') }}</label>
          <select v-model="hsVariant">
            <option v-for="v in VARIANTS" :key="v" :value="v">{{ VARIANT_LABELS[v][lang] }}</option>
          </select>
        </div>
        <div class="group">
          <label>{{ t('three_resolution') }}</label>
          <input type="number" v-model.number="hsRes" min="32" max="4096" step="64" />
        </div>
        <div class="group">
          <label>{{ t('iterations') }}</label>
          <input type="number" v-model.number="hsIter" min="64" max="10000" step="128" />
        </div>
        <div v-if="hsMetric === 'min_pairwise_dist'" class="group">
          <label>{{ lang === 'en' ? 'PAIRWISE CAP' : '成对距离上限' }}</label>
          <input type="number" v-model.number="hsPairwiseCap" min="1" max="1000000" step="64" />
          <span class="num dim">O({{ Math.min(hsIter, hsPairwiseCap) }}²) / {{ lang === 'en' ? 'px' : '像素' }}</span>
        </div>
        <div class="group">
          <label>{{ t('three_center_re') }}</label>
          <input type="number" v-model.number="hsCenterRe" step="0.01" />
        </div>
        <div class="group">
          <label>{{ t('three_center_im') }}</label>
          <input type="number" v-model.number="hsCenterIm" step="0.01" />
        </div>
        <div class="group">
          <label>{{ t('three_scale') }}</label>
          <input type="number" v-model.number="hsScale" min="0.0001" step="0.1" />
        </div>

        <!-- Z-scale controls -->
        <div class="rule"></div>
        <div class="group">
          <label>{{ lang === 'en' ? 'Z-SCALE' : 'Z 轴缩放' }}</label>
          <div class="zscale-row">
            <label class="inline-label">
              <input type="checkbox" :checked="hsZSign === -1" @change="hsZSign = (hsZSign === 1 ? -1 : 1)" />
              {{ lang === 'en' ? 'Concave' : '凹面' }}
            </label>
            <span class="num">{{ (hsZScale >= 0 ? '' : '−') + Math.pow(10, hsZExp).toExponential(1) }}</span>
          </div>
          <input type="range" min="-15" max="0" step="0.1" v-model.number="hsZExp" />
          <span class="num dim">10<sup>{{ hsZExp.toFixed(1) }}</sup></span>
        </div>

        <!-- STL export -->
        <button class="stl-btn" @click="exportHsStl" :disabled="stlLoading">
          {{ stlLoading ? (lang === 'en' ? 'Exporting…' : '导出中…') : (lang === 'en' ? 'Export STL' : '导出 STL') }}
        </button>
        <a v-if="stlUrl" :href="stlUrl" download class="stl-link mono">
          {{ lang === 'en' ? '⬇ download STL' : '⬇ 下载 STL' }}
        </a>
      </template>

      <!-- Transition params (voxel only) -->
      <template v-else>
        <div class="group">
          <label>{{ t('variant') }} A</label>
          <select v-model="txFrom">
            <option v-for="v in AXIS_TRANSITION_VARIANTS" :key="'tx-from-' + v" :value="v">{{ VARIANT_LABELS[v][lang] }}</option>
          </select>
        </div>
        <div class="group">
          <label>{{ t('variant') }} B</label>
          <select v-model="txTo">
            <option v-for="v in AXIS_TRANSITION_VARIANTS" :key="'tx-to-' + v" :value="v">{{ VARIANT_LABELS[v][lang] }}</option>
          </select>
        </div>
        <div class="group">
          <label>{{ t('three_iso') }} <span class="dim">{{ lang === 'en' ? '(core depth)' : '（核心深度）' }}</span></label>
          <input type="range" min="0.05" max="0.48" step="0.01" v-model.number="txIso" />
          <span class="num">{{ txIso.toFixed(2) }}{{ txIso >= 0.45 ? (lang === 'en' ? ' (all)' : '（全部）') : '' }}</span>
        </div>
        <div class="group">
          <label>{{ t('three_resolution') }}</label>
          <input type="number" v-model.number="txRes" min="16" step="16" />
          <span class="num dim">{{ txRes }}³ vox</span>
        </div>
        <div class="group">
          <label>{{ t('iterations') }}</label>
          <input type="number" v-model.number="txIter" min="32" max="2000" step="32" />
        </div>

        <!-- Center and extent -->
        <div class="rule"></div>
        <div class="group">
          <label>{{ lang === 'en' ? 'CENTER X' : '中心 X' }}</label>
          <input type="number" v-model.number="txCenterX" step="0.1" />
        </div>
        <div class="group">
          <label>{{ lang === 'en' ? 'CENTER Y' : '中心 Y' }}</label>
          <input type="number" v-model.number="txCenterY" step="0.1" />
        </div>
        <div class="group">
          <label>{{ lang === 'en' ? 'CENTER Z' : '中心 Z' }}</label>
          <input type="number" v-model.number="txCenterZ" step="0.1" />
        </div>
        <div class="group">
          <label>{{ lang === 'en' ? 'EXTENT' : '范围' }}</label>
          <input type="number" v-model.number="txExtent" min="0.1" step="0.1" />
        </div>

        <!-- STL export -->
        <div class="rule"></div>
        <button class="stl-btn" @click="exportTxStl" :disabled="stlLoading">
          {{ stlLoading ? (lang === 'en' ? 'Exporting…' : '导出中…') : (lang === 'en' ? 'Export STL' : '导出 STL') }}
        </button>
        <a v-if="stlUrl" :href="stlUrl" download class="stl-link mono">
          {{ lang === 'en' ? '⬇ download STL' : '⬇ 下载 STL' }}
        </a>
      </template>

      <button class="compute-btn" @click="compute" :disabled="loading">
        {{ loading ? t('three_computing') : t('three_compute') }}
      </button>

      <div v-if="info"  class="info mono">{{ info }}</div>
      <div v-if="error" class="err mono">{{ error }}</div>
    </aside>

    <!-- Viewer canvas -->
    <div class="viewer">
      <ThreeDViewer
        :glbUrl="null"
        :voxelData="voxelData"
        :hsFieldData="hsFieldData"
        :zScale="hsZScale"
        :viewMode="mode"
        :loading="loading"
        :extent="mode === 'transition' ? txExtent : undefined"
        @pan="onHsPan"
        @zoom="onHsZoom" />
    </div>
  </div>
</template>

<style scoped>
.three-view {
  display: grid;
  grid-template-columns: 240px 1fr;
  height: 100%;
  overflow: hidden;
}

.controls {
  display: flex;
  flex-direction: column;
  gap: 10px;
  padding: 14px 12px;
  border-right: 1px solid var(--rule);
  background: var(--panel);
  overflow-y: auto;
}

.mode-row {
  display: flex;
  gap: 6px;
  margin-bottom: 6px;
}

.mode-btn {
  flex: 1;
  padding: 5px;
  background: var(--rule);
  color: var(--text-dim);
  border: 1px solid transparent;
  font-family: var(--mono);
  font-size: 11px;
  cursor: pointer;
  transition: border-color 0.1s, color 0.1s;
}

.mode-btn.active {
  border-color: var(--accent);
  color: var(--accent);
}

.group {
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.group label {
  font-size: 10px;
  text-transform: uppercase;
  letter-spacing: 0.07em;
  color: var(--text-dim);
}

.group select,
.group input[type="number"] {
  width: 100%;
}

.group input[type="range"] { width: 100%; }

.zscale-row {
  display: flex;
  justify-content: space-between;
  align-items: center;
  gap: 6px;
}

.inline-label {
  display: flex;
  align-items: center;
  gap: 4px;
  font-size: 11px;
  color: var(--text-dim);
  text-transform: none;
  letter-spacing: normal;
  cursor: pointer;
}

.num {
  font-family: var(--mono);
  font-size: 11px;
  color: var(--text-dim);
  align-self: flex-end;
}

.dim { color: var(--text-faint); }

.compute-btn {
  margin-top: 8px;
  padding: 8px;
  background: var(--accent);
  color: #000;
  font-family: var(--mono);
  font-size: 12px;
  border: none;
  cursor: pointer;
  font-weight: 600;
  letter-spacing: 0.05em;
}

.compute-btn:disabled {
  opacity: 0.5;
  cursor: default;
}

.stl-btn {
  padding: 6px 8px;
  background: transparent;
  color: var(--text-dim);
  border: 1px solid var(--rule);
  font-family: var(--mono);
  font-size: 11px;
  cursor: pointer;
  text-transform: uppercase;
  letter-spacing: 0.05em;
}

.stl-btn:hover:not(:disabled) { border-color: var(--accent-edge); color: var(--accent); }
.stl-btn:disabled { opacity: 0.5; cursor: default; }

.stl-link {
  font-size: 11px;
  color: var(--accent);
  text-decoration: none;
  padding: 3px 0;
}

.info, .err {
  font-size: 10px;
  line-height: 1.4;
  color: var(--text-dim);
  word-break: break-all;
  margin-top: 4px;
}

.err { color: var(--bad); }

.rule {
  border-top: 1px solid var(--rule);
  margin: 4px 0;
}

.viewer {
  position: relative;
  min-height: 0;
  height: 100%;
}

@media (max-width: 760px), ((pointer: coarse) and (max-width: 1200px)), ((any-pointer: coarse) and (max-width: 1200px)), ((min-width: 761px) and (max-width: 1200px) and (orientation: landscape)) {
  .three-view {
    grid-template-columns: 1fr;
    grid-template-rows: minmax(220px, 40dvh) minmax(0, 1fr);
  }

  .controls {
    border-right: none;
    border-bottom: 1px solid var(--rule);
    display: grid;
    grid-template-columns: 1fr;
    align-content: start;
    padding: 10px;
  }

  .mode-row,
  .rule,
  .compute-btn,
  .stl-btn,
  .stl-link,
  .info,
  .err {
    grid-column: 1 / -1;
  }

  .viewer {
    min-height: 280px;
  }
}

@media (min-width: 761px) and (max-width: 1200px) and (orientation: landscape) {
  .three-view {
    grid-template-columns: minmax(240px, 28vw) minmax(0, 1fr);
    grid-template-rows: minmax(0, 1fr);
  }

  .controls {
    display: flex;
    flex-direction: column;
    border-right: 1px solid var(--rule);
    border-bottom: none;
    padding: 12px;
  }

  .viewer {
    min-height: 0;
  }
}



</style>
