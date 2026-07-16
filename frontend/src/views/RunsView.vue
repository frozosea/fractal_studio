<script setup lang="ts">
import { onMounted, ref, watch } from 'vue'
import { api, type RunRow, type ArtifactRow } from '../api'
import { t, lang } from '../i18n'

// ── Run categories ──────────────────────────────────────────────────────────
// Group the raw module names (15+) into a handful of meaningful categories so the 8k+ runs
// are browsable by what they actually are. "Other" is the catch-all (anything uncategorized,
// including empty-module runs).
interface Category {
  key: string
  label: { en: string; zh: string }
  modules: string[]   // empty = "all" (key 'all') or "other" (computed)
  color: string
}
// Mirrors categoryForModule() in backend/src/core/job_runner.cpp — each chip is one on-disk
// runs/<category>/ folder. Keep the two in sync.
const CATEGORIES: Category[] = [
  { key: 'all',       label: { en: 'All',      zh: '全部' },   modules: [], color: 'var(--text-dim)' },
  { key: 'videos',    label: { en: 'Videos',   zh: '视频' },   modules: ['video-export', 'zoom-video', 'video-preview', 'transition-video-export', 'transition-video-preview'], color: '#ff8a5b' },
  { key: 'maps',      label: { en: 'Maps',     zh: '映射' },   modules: ['map', 'map-export'], color: '#5bc8ff' },
  { key: 'ln-maps',   label: { en: 'Ln-maps',  zh: 'Ln-map' }, modules: ['ln-map'], color: '#5be0c8' },
  { key: 'frames',    label: { en: 'Frames',   zh: '帧' },     modules: ['start-frame'], color: '#c8e05b' },
  { key: 'meshes',    label: { en: 'Meshes',   zh: '网格' },   modules: ['hs-mesh', 'hs-field', 'transition-mesh', 'transition-voxels'], color: '#7fd66a' },
  { key: 'points',    label: { en: 'Points',   zh: '特殊点' }, modules: ['special-points-search', 'special-points-enumerate'], color: '#c89bff' },
  { key: 'benchmark', label: { en: 'Benchmark', zh: '基准' },  modules: ['benchmark'], color: '#ffd24d' },
  { key: 'other',     label: { en: 'Other',    zh: '其他' },   modules: [], color: '#9aa' },
]
const categorizedModules = new Set(CATEGORIES.flatMap(c => c.modules))
const moduleToCategory: Record<string, Category> = {}
for (const c of CATEGORIES) for (const m of c.modules) moduleToCategory[m] = c
function categoryOf(module: string): Category {
  return moduleToCategory[module] ?? CATEGORIES[CATEGORIES.length - 1]  // 'other'
}

const runs = ref<RunRow[]>([])
const modules = ref<string[]>([])
const totalCount = ref(0)
const activeCategory = ref('all')
const statusFilter = ref('')
const pageSize = 50
const offset = ref(0)
const loading = ref(false)
const expandedRun = ref('')
const runArtifactsMap = ref<Record<string, ArtifactRow[]>>({})

// Module-filter string sent to the API for the active category. A category spans several
// modules → comma-joined IN list. "Other" = available modules not in any category (includes
// the empty-module runs; a lone empty value is sent as ',' so the backend doesn't read it as
// "no filter").
function categoryModuleParam(): string | undefined {
  if (activeCategory.value === 'all') return undefined
  if (activeCategory.value === 'other') {
    const others = modules.value.filter(m => !categorizedModules.has(m))
    if (others.length === 0) return undefined
    const joined = others.join(',')
    return joined === '' ? ',' : joined
  }
  const cat = CATEGORIES.find(c => c.key === activeCategory.value)
  return cat ? cat.modules.join(',') : undefined
}

async function fetchRuns(off: number) {
  return api.runs({
    limit: pageSize,
    offset: off,
    module: categoryModuleParam(),
    status: statusFilter.value || undefined,
  })
}

async function refresh() {
  loading.value = true
  offset.value = 0
  try {
    const r = await fetchRuns(0)
    runs.value = r.items
    totalCount.value = r.totalCount
    modules.value = r.modules
  } finally {
    loading.value = false
  }
}

async function loadMore() {
  loading.value = true
  try {
    const nextOffset = offset.value + pageSize
    const r = await fetchRuns(nextOffset)
    runs.value = [...runs.value, ...r.items]
    offset.value = nextOffset
    totalCount.value = r.totalCount
  } finally {
    loading.value = false
  }
}

function fmtTime(ms: number) {
  if (!ms) return '—'
  return new Date(ms).toLocaleString()
}

function fmtDuration(row: RunRow) {
  if (!row.finishedAt) return '—'
  const ms = row.finishedAt - row.startedAt
  if (ms < 1000) return ms + 'ms'
  const sec = Math.round(ms / 1000)
  if (sec < 60) return sec + 's'
  const m = Math.floor(sec / 60)
  const s = sec % 60
  return m + 'm ' + s + 's'
}

function dateKey(ms: number): string {
  if (!ms) return ''
  return new Date(ms).toLocaleDateString()
}

function dateLabel(ms: number): string {
  if (!ms) return ''
  const d = new Date(ms)
  const now = new Date()
  const yesterday = new Date(now)
  yesterday.setDate(yesterday.getDate() - 1)
  if (d.toDateString() === now.toDateString()) return lang.value === 'en' ? 'Today' : '今天'
  if (d.toDateString() === yesterday.toDateString()) return lang.value === 'en' ? 'Yesterday' : '昨天'
  return d.toLocaleDateString()
}

function showDateSeparator(idx: number): string | null {
  if (idx === 0) return dateLabel(runs.value[0].startedAt)
  const prev = dateKey(runs.value[idx - 1].startedAt)
  const cur = dateKey(runs.value[idx].startedAt)
  if (prev !== cur) return dateLabel(runs.value[idx].startedAt)
  return null
}

async function toggleRunExpand(runId: string) {
  if (expandedRun.value === runId) {
    expandedRun.value = ''
    return
  }
  expandedRun.value = runId
  if (!runArtifactsMap.value[runId]) {
    try {
      const r = await api.artifacts(undefined, runId)
      runArtifactsMap.value[runId] = r.items
    } catch {
      runArtifactsMap.value[runId] = []
    }
  }
}

function isImage(a: ArtifactRow) {
  return a.kind === 'image' || /\.(png|jpg|jpeg|webp|bmp)$/i.test(a.name)
}

function isVideo(a: ArtifactRow) {
  return a.kind === 'video' || /\.(mp4|webm|mkv|avi)$/i.test(a.name)
}

const hasMore = () => runs.value.length < totalCount.value

watch([activeCategory, statusFilter], () => { refresh() })

onMounted(refresh)
</script>

<template>
  <div class="wrap">
    <div class="head">
      <span class="panel-title">{{ t('runs_title') }} <span class="count">({{ runs.length }} / {{ totalCount }})</span></span>
      <div class="filters">
        <select v-model="statusFilter" class="filter-select">
          <option value="">{{ lang === 'en' ? 'all status' : '全部状态' }}</option>
          <option value="completed">{{ lang === 'en' ? 'completed' : '已完成' }}</option>
          <option value="failed">{{ lang === 'en' ? 'failed' : '失败' }}</option>
          <option value="cancelled">{{ lang === 'en' ? 'cancelled' : '已取消' }}</option>
          <option value="running">{{ lang === 'en' ? 'running' : '运行中' }}</option>
        </select>
        <button @click="refresh" :disabled="loading" class="btn-refresh">↻</button>
      </div>
    </div>

    <div class="category-bar">
      <button v-for="c in CATEGORIES" :key="c.key"
              class="cat-chip" :class="{ active: activeCategory === c.key }"
              :style="activeCategory === c.key ? { borderColor: c.color, color: c.color } : {}"
              @click="activeCategory = c.key">
        <span v-if="c.key !== 'all'" class="cat-dot" :style="{ background: c.color }"></span>
        {{ c.label[lang] }}
      </button>
    </div>

    <div class="runs-list">
      <template v-for="(r, idx) in runs" :key="r.id">
        <div v-if="showDateSeparator(idx)" class="date-separator">
          {{ showDateSeparator(idx) }}
        </div>
        <div class="run-row" :class="{ expanded: expandedRun === r.id }" @click="toggleRunExpand(r.id)">
          <div class="run-main">
            <span class="run-id mono dim">{{ r.id }}</span>
            <span class="run-cat" :title="r.module || '—'">
              <span class="cat-dot" :style="{ background: categoryOf(r.module).color }"></span>
              <span class="run-module mono">{{ r.module || '—' }}</span>
            </span>
            <span class="run-status mono" :class="{ good: r.status === 'completed', bad: r.status === 'failed' }">{{ r.status }}</span>
            <span class="run-time mono dim">{{ fmtTime(r.startedAt) }}</span>
            <span class="run-duration mono">{{ fmtDuration(r) }}</span>
          </div>
        </div>
        <div v-if="expandedRun === r.id" class="run-detail">
          <div v-if="!runArtifactsMap[r.id]" class="loading-artifacts">{{ lang === 'en' ? 'loading…' : '加载中…' }}</div>
          <div v-else-if="runArtifactsMap[r.id].length === 0" class="no-artifacts">{{ lang === 'en' ? 'no artifacts' : '无产物' }}</div>
          <div v-else class="artifact-grid">
            <a v-for="a in runArtifactsMap[r.id]" :key="a.artifactId"
               :href="api.artifactDownloadUrl(a.artifactId)" target="_blank" class="artifact-card">
              <img v-if="isImage(a)" :src="api.artifactContentUrl(a.artifactId)" loading="lazy" class="artifact-thumb" />
              <div v-else-if="isVideo(a)" class="artifact-video-badge">{{ lang === 'en' ? '▶ video' : '▶ 视频' }}</div>
              <div v-else class="artifact-file-badge">{{ a.kind }}</div>
              <span class="artifact-name mono">{{ a.name }}</span>
              <span class="artifact-size dim">{{ (a.sizeBytes / 1024).toFixed(1) }} KiB</span>
            </a>
          </div>
        </div>
      </template>
    </div>

    <div v-if="hasMore()" class="load-more">
      <button @click="loadMore" :disabled="loading">
        {{ loading ? '…' : (lang === 'en' ? 'Load more' : '加载更多') }}
      </button>
    </div>
  </div>
</template>

<style scoped>
.wrap { padding: 18px 22px; overflow: auto; height: 100%; }

.head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 14px;
  flex-wrap: wrap;
  gap: 8px;
}

.count { color: var(--text-dim); font-weight: normal; }

.filters {
  display: flex;
  gap: 6px;
  align-items: center;
}

.filter-select {
  font-size: var(--fs-mono);
  font-family: var(--mono);
  padding: 3px 6px;
  background: var(--bg);
  color: var(--text);
  border: 1px solid var(--rule);
}

.btn-refresh {
  padding: 3px 8px;
  font-size: 14px;
  line-height: 1;
}

.category-bar {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
  margin-bottom: 12px;
}
.cat-chip {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  padding: 4px 12px;
  font-size: var(--fs-mono);
  font-family: var(--mono);
  background: var(--bg);
  color: var(--text-dim);
  border: 1px solid var(--rule);
  border-radius: 14px;
  cursor: pointer;
}
.cat-chip:hover { background: var(--bg-raised); }
.cat-chip.active { background: var(--accent-weak); font-weight: 600; }
.cat-dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  flex-shrink: 0;
}
.run-cat {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  overflow: hidden;
}

.date-separator {
  font-size: 11px;
  color: var(--accent);
  text-transform: uppercase;
  letter-spacing: 0.08em;
  padding: 12px 0 4px;
  border-bottom: 1px solid var(--rule);
  margin-top: 4px;
}

.runs-list { display: flex; flex-direction: column; }

.run-row {
  cursor: pointer;
  border-bottom: 1px solid var(--rule);
  padding: 6px 0;
}
.run-row:hover { background: var(--bg-raised); }
.run-row.expanded { background: var(--accent-weak); }

.run-main {
  display: grid;
  grid-template-columns: 1fr auto auto 1fr auto;
  gap: 10px;
  align-items: center;
  font-size: var(--fs-mono);
}

.run-id { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.run-module { min-width: 80px; }
.run-status { min-width: 70px; text-align: center; }
.run-time { text-align: right; }
.run-duration { min-width: 60px; text-align: right; }

.mono { font-family: var(--mono); }
.dim { color: var(--text-dim); }
.good { color: var(--good); }
.bad  { color: var(--bad); }

.run-detail {
  padding: 10px 8px 14px;
  border-bottom: 1px solid var(--rule);
  background: var(--panel);
}

.loading-artifacts, .no-artifacts {
  font-size: var(--fs-mono);
  color: var(--text-dim);
  font-family: var(--mono);
}

.artifact-grid {
  display: flex;
  flex-wrap: wrap;
  gap: 10px;
}

.artifact-card {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 4px;
  padding: 6px;
  border: 1px solid var(--rule);
  background: var(--bg);
  min-width: 80px;
  max-width: 140px;
  text-decoration: none;
  color: inherit;
}
.artifact-card:hover { border-color: var(--accent); }

.artifact-thumb {
  max-width: 120px;
  max-height: 90px;
  object-fit: contain;
  image-rendering: auto;
}

.artifact-video-badge, .artifact-file-badge {
  width: 80px;
  height: 50px;
  display: flex;
  align-items: center;
  justify-content: center;
  background: var(--bg-raised);
  color: var(--text-dim);
  font-family: var(--mono);
  font-size: 11px;
}

.artifact-name {
  font-size: 10px;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  max-width: 120px;
  text-align: center;
}

.artifact-size {
  font-size: 9px;
  font-family: var(--mono);
}

.load-more {
  display: flex;
  justify-content: center;
  padding: 14px 0;
}

.load-more button {
  padding: 6px 24px;
  font-family: var(--mono);
  font-size: var(--fs-mono);
}

@media (max-width: 760px), ((pointer: coarse) and (max-width: 1200px)), ((any-pointer: coarse) and (max-width: 1200px)), ((min-width: 761px) and (max-width: 1200px) and (orientation: landscape)) {
  .wrap { padding: 12px; }

  .head { gap: 6px; }

  .run-main {
    grid-template-columns: 1fr auto auto;
    font-size: 11px;
  }

  .run-time, .run-duration { display: none; }

  .artifact-grid { gap: 6px; }

  .artifact-card { min-width: 60px; max-width: 100px; padding: 4px; }
  .artifact-thumb { max-width: 80px; max-height: 60px; }
}
</style>
