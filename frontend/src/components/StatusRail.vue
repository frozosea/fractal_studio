<script setup lang="ts">
import { onMounted, onUnmounted, ref } from 'vue'
import { api } from '../api'
import type { ActiveTask, ResourceLockStatus } from '../api'
import type { StatusState } from '../types'
import { t, lang, setLang } from '../i18n'

defineProps<{
  status: StatusState
  collapsed?: boolean
}>()
defineEmits<{ (e: 'toggle'): void }>()

const hw = ref<any>(null)
const tasks = ref<ActiveTask[]>([])
const locks = ref<ResourceLockStatus[]>([])
let interval: any
let taskInterval: any

async function refresh() {
  try {
    hw.value = await api.hardware()
  } catch {}
}

async function refreshTasks() {
  try {
    const r = await api.activeTasks()
    tasks.value = r.items
    locks.value = r.resourceLocks
  } catch {}
}

async function cancelTask(runId: string) {
  try {
    await api.cancelRun(runId)
    await refreshTasks()
  } catch {}
}

onMounted(() => {
  refresh()
  refreshTasks()
  interval = setInterval(refresh, 10000)
  taskInterval = setInterval(refreshTasks, 1500)
})

onUnmounted(() => {
  if (interval) clearInterval(interval)
  if (taskInterval) clearInterval(taskInterval)
})

function fmt(n: number | null, digits = 6): string {
  if (n === null || !isFinite(n)) return '—'
  return n.toFixed(digits)
}

function fmtSci(n: number | null): string {
  if (n === null || !isFinite(n)) return '—'
  return n.toExponential(3)
}

function fmtMs(n: number | null): string {
  if (n === null || !isFinite(n)) return '—'
  return n.toFixed(1) + 'ms'
}

function fmtElapsed(n?: number): string {
  if (!n || !isFinite(n)) return '0s'
  if (n < 1000) return `${Math.round(n)}ms`
  const s = Math.round(n / 1000)
  if (s < 60) return `${s}s`
  const m = Math.floor(s / 60)
  return `${m}m ${s % 60}s`
}

function taskPercent(task: ActiveTask): string {
  const p = task.progress?.percent
  if (typeof p === 'number' && isFinite(p)) return `${Math.max(0, Math.min(100, p)).toFixed(0)}%`
  const c = task.progress?.current
  const total = task.progress?.total
  if (typeof c === 'number' && typeof total === 'number' && total > 0) return `${Math.round(c / total * 100)}%`
  return '—'
}

function taskEta(task: ActiveTask): string {
  const eta = task.progress?.estimatedRemainingMs
  return typeof eta === 'number' && isFinite(eta) && eta >= 0 ? fmtElapsed(eta) : '—'
}
</script>

<template>
  <aside :class="['rail', { collapsed }]">
    <button
      class="rail-toggle"
      :title="collapsed ? 'Expand system status / 展开系统状态' : 'Collapse system status / 折叠系统状态'"
      @click="$emit('toggle')">
      <span class="toggle-mark">{{ collapsed ? '‹' : '›' }}</span>
    </button>

    <div
      v-if="collapsed"
      class="collapsed-summary"
      :title="`${t('status_time')}: ${fmtMs(status.renderMs)}`">
      <span class="collapsed-label">{{ t('status_time') }}</span>
      <span class="collapsed-time num">{{ fmtMs(status.renderMs) }}</span>
    </div>

    <template v-else>
    <!-- live bar -->
    <div class="live">
      <span class="dot"></span>
      <span class="mono">{{ status.message }}</span>
      <span class="spacer"></span>
      <span class="lang" @click="setLang(lang === 'en' ? 'zh' : 'en')">{{ lang.toUpperCase() }}</span>
    </div>

    <!-- engine + timing -->
    <div class="panel">
      <div class="panel-title">engine</div>
      <div class="row"><span class="k">engine</span><span class="v mono">{{ status.engine }}</span></div>
      <div class="row"><span class="k">{{ t('status_time') }}</span><span class="v num">{{ fmtMs(status.renderMs) }}</span></div>
    </div>

    <!-- viewport numerics -->
    <div class="panel">
      <div class="panel-title">viewport</div>
      <div class="row"><span class="k">c.re</span><span class="v num">{{ fmt(status.cRe, 10) }}</span></div>
      <div class="row"><span class="k">c.im</span><span class="v num">{{ fmt(status.cIm, 10) }}</span></div>
      <div class="row"><span class="k">zoom</span><span class="v num">{{ fmtSci(status.zoom) }}</span></div>
      <div class="row"><span class="k">iter</span><span class="v num">{{ status.iter ?? '—' }}</span></div>
      <div class="row"><span class="k">variant</span><span class="v mono">{{ status.variant }}</span></div>
      <div class="row"><span class="k">metric</span><span class="v mono">{{ status.metric }}</span></div>
    </div>

    <!-- hardware -->
    <div class="panel">
      <div class="panel-title">hardware</div>
      <div v-if="hw">
        <div class="row"><span class="k">cpu</span><span class="v mono hwcpu">{{ hw.cpuModel }}</span></div>
        <div class="row"><span class="k">cores</span><span class="v num">{{ hw.cpuPhysicalCores }} / {{ hw.cpuLogicalCores }}</span></div>
        <div class="row"><span class="k">mem</span><span class="v num">{{ Math.round(hw.memoryAvailableMiB/1024) }} / {{ Math.round(hw.memoryTotalMiB/1024) }} GiB</span></div>
        <div class="row"><span class="k">gpu</span><span class="v mono hwcpu">{{ hw.gpuModel }}</span></div>
        <div class="row"><span class="k">vram</span><span class="v mono">{{ hw.gpuMemory }}</span></div>
      </div>
    </div>

    <div class="panel">
      <div class="panel-title">active tasks</div>
      <div class="locks">
        <span v-for="lock in locks" :key="lock.name" :class="['lock', lock.busy ? 'busy' : 'idle']">
          {{ lock.name }}
        </span>
      </div>
      <div v-if="tasks.length === 0" class="empty">idle</div>
      <div v-for="task in tasks" :key="task.runId" class="task">
        <div class="task-head">
          <span class="mono task-type">{{ task.taskType }}</span>
          <button v-if="task.cancelable" class="cancel" @click="cancelTask(task.runId)">cancel</button>
        </div>
        <div class="row"><span class="k">stage</span><span class="v mono">{{ task.stage || task.status }}</span></div>
        <div class="row"><span class="k">engine</span><span class="v mono">{{ task.engine || '—' }}</span></div>
        <div class="row"><span class="k">progress</span><span class="v num">{{ taskPercent(task) }}</span></div>
        <div class="bar"><span :style="{ width: taskPercent(task) === '—' ? '0%' : taskPercent(task) }"></span></div>
        <div class="row"><span class="k">elapsed</span><span class="v num">{{ fmtElapsed(task.elapsedMs) }}</span></div>
        <div class="row"><span class="k">eta</span><span class="v num">{{ taskEta(task) }}</span></div>
      </div>
    </div>
    </template>
  </aside>
</template>

<style scoped>
.rail {
  background: var(--bg);
  overflow: auto;
  display: flex;
  flex-direction: column;
  gap: 1px;
}

.rail.collapsed {
  overflow: hidden;
  align-items: center;
  padding-top: 8px;
}

.rail-toggle {
  width: 20px;
  height: 24px;
  min-width: 20px;
  padding: 0;
  margin: 8px 4px 0;
  border-color: transparent;
  color: var(--text-faint);
}

.rail.collapsed .rail-toggle {
  margin-top: 0;
}

.rail-toggle:hover {
  border-color: var(--rule-hi);
  color: var(--accent);
}

.toggle-mark {
  display: block;
  font-size: 16px;
  line-height: 1;
  letter-spacing: 0;
}

.collapsed-summary {
  display: flex;
  align-items: center;
  gap: 8px;
  margin-top: 10px;
  color: var(--text-dim);
  writing-mode: vertical-rl;
  text-orientation: mixed;
  font-family: var(--mono);
}

.collapsed-label {
  color: var(--text-faint);
  font-size: 9px;
  text-transform: uppercase;
  letter-spacing: 0.08em;
}

.collapsed-time {
  color: var(--accent);
  font-size: 10px;
  letter-spacing: 0.02em;
}

.live {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 10px 14px;
  border-bottom: 1px solid var(--rule);
  font-size: var(--fs-label);
  color: var(--text-dim);
  text-transform: uppercase;
  letter-spacing: 0.1em;
}

.dot {
  width: 6px; height: 6px;
  background: var(--good);
  border-radius: 50%;
  box-shadow: 0 0 6px var(--good);
}

.spacer { flex: 1; }

.lang {
  cursor: pointer;
  color: var(--accent);
  font-family: var(--mono);
}

.panel {
  background: var(--panel);
  border: none;
  border-bottom: 1px solid var(--rule);
  padding: 12px 14px;
}

.row {
  display: grid;
  grid-template-columns: 56px 1fr;
  align-items: baseline;
  padding: 2px 0;
  font-size: var(--fs-mono);
}

.k {
  color: var(--text-faint);
  font-family: var(--mono);
  font-size: var(--fs-label);
  text-transform: uppercase;
  letter-spacing: 0.08em;
}

.v {
  color: var(--text);
  text-align: right;
}

.num { font-variant-numeric: tabular-nums; }

.hwcpu {
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
  font-size: 10px;
}

.locks {
  display: flex;
  flex-wrap: wrap;
  gap: 4px;
  margin-bottom: 8px;
}

.lock {
  border: 1px solid var(--rule);
  color: var(--text-faint);
  font-family: var(--mono);
  font-size: 9px;
  padding: 2px 5px;
  text-transform: uppercase;
}

.lock.busy {
  color: var(--warn);
  border-color: color-mix(in srgb, var(--warn) 45%, var(--rule));
}

.empty {
  color: var(--text-faint);
  font-family: var(--mono);
  font-size: var(--fs-label);
  text-transform: uppercase;
}

.task {
  border-top: 1px solid var(--rule);
  padding-top: 8px;
  margin-top: 8px;
}

.task-head {
  display: flex;
  align-items: center;
  gap: 8px;
  margin-bottom: 4px;
}

.task-type {
  flex: 1;
  color: var(--text);
  font-size: 10px;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.cancel {
  border: 1px solid var(--rule);
  background: transparent;
  color: var(--text-dim);
  font-family: var(--mono);
  font-size: 10px;
  padding: 3px 6px;
  cursor: pointer;
}

.cancel:hover {
  border-color: var(--bad);
  color: var(--bad);
}

.bar {
  height: 3px;
  background: var(--bg-raised);
  border-radius: 2px;
  overflow: hidden;
  margin: 3px 0 5px;
}

.bar span {
  display: block;
  height: 100%;
  background: var(--accent);
}









@media (max-width: 760px), ((pointer: coarse) and (max-width: 1200px)), ((any-pointer: coarse) and (max-width: 1200px)) {
  .rail {
    max-height: min(42dvh, 320px);
    border-top: 1px solid var(--rule);
  }

  .rail.collapsed {
    min-height: 30px;
    max-height: 30px;
    flex-direction: row;
    align-items: center;
    justify-content: flex-start;
    padding: 0 8px;
    overflow: hidden;
  }

  .rail-toggle,
  .rail.collapsed .rail-toggle {
    margin: 0 6px 0 0;
    flex: 0 0 24px;
    width: 24px;
    min-width: 24px;
  }

  .collapsed-summary {
    writing-mode: horizontal-tb;
    text-orientation: mixed;
    margin-top: 0;
    min-width: 0;
    flex: 1;
  }

  .live {
    padding: 8px 10px;
  }

  .panel {
    padding: 10px 12px;
  }

  .row {
    grid-template-columns: 52px minmax(0, 1fr);
  }

  .v {
    min-width: 0;
    overflow-wrap: anywhere;
  }
}
</style>
