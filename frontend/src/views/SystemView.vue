<script setup lang="ts">
import { onMounted, ref } from 'vue'
import { api, type Hardware } from '../api'
import { t } from '../i18n'

const hw    = ref<Hardware | null>(null)
const check = ref<{ openmp: boolean; cuda: boolean } | null>(null)
const caps  = ref<Record<string, any> | null>(null)
const loading = ref(false)

async function refresh() {
  loading.value = true
  try {
    hw.value    = await api.hardware()
    check.value = await api.systemCheck()
    caps.value  = await api.capabilities()
  } finally {
    loading.value = false
  }
}

onMounted(refresh)

// ---- Benchmark ----

interface BenchResult {
  engine: string
  scalar: string
  available: boolean
  elapsedMs: number
  mpixPerSec: number
}

const benchRunning = ref(false)
const benchResults = ref<BenchResult[]>([])
const benchError   = ref('')
const benchIters   = ref(2000)
const benchSize    = ref(512)

async function runBenchmark() {
  benchRunning.value = true
  benchError.value   = ''
  benchResults.value = []
  try {
    const data = await api.benchmark({
      width: benchSize.value,
      height: benchSize.value,
      iterations: benchIters.value,
      centerRe: -0.75,
      centerIm: 0,
      scale: 1.5,
    })
    benchResults.value = data.results ?? []
  } catch (e: any) {
    benchError.value = e?.data?.error ?? e?.message ?? String(e)
  } finally {
    benchRunning.value = false
  }
}

function speedBar(mpps: number): string {
  // Normalise to a width based on max observed
  const max = Math.max(...benchResults.value.map(r => r.mpixPerSec), 0.01)
  return `${Math.min(100, (mpps / max) * 100).toFixed(1)}%`
}
</script>

<template>
  <div class="wrap">
    <div class="head">
      <span class="panel-title">{{ t('sys_title') }}</span>
      <button @click="refresh" :disabled="loading">{{ t('render') }}</button>
    </div>

    <div class="grid">
      <div class="panel">
        <div class="panel-title">{{ t('sys_engines') }}</div>
        <div v-if="check">
          <div class="row">
            <span class="k">openmp</span>
            <span class="v mono" :class="{ good: check.openmp, bad: !check.openmp }">
              {{ check.openmp ? 'available' : 'unavailable' }}
            </span>
          </div>
          <div class="row">
            <span class="k">cuda</span>
            <span class="v mono" :class="{ good: check.cuda, bad: !check.cuda }">
              {{ check.cuda ? 'available' : 'unavailable' }}
            </span>
          </div>
          <div class="row">
            <span class="k">avx2/fma</span>
            <span class="v mono" :class="{ good: caps?.avx2?.runtime, bad: !caps?.avx2?.runtime }">
              {{ caps?.avx2?.runtime ? 'available' : 'unavailable' }}
            </span>
          </div>
          <div class="row">
            <span class="k">avx-512</span>
            <span class="v mono" :class="{ good: caps?.avx512?.runtime, bad: !caps?.avx512?.runtime }">
              {{ caps?.avx512?.runtime ? 'available' : 'unavailable' }}
            </span>
          </div>
          <div class="row">
            <span class="k">fx64</span>
            <span class="v mono good">enabled (auto &lt;1e-13)</span>
          </div>
          <div class="row">
            <span class="k">hybrid</span>
            <span class="v mono good">cpu+gpu scheduler</span>
          </div>
        </div>
      </div>

      <div class="panel">
        <div class="panel-title">{{ t('sys_title') }}</div>
        <div v-if="hw">
          <div class="row"><span class="k">{{ t('sys_cpu') }}</span><span class="v mono">{{ hw.cpuModel }}</span></div>
          <div class="row"><span class="k">cores</span><span class="v num">{{ hw.cpuPhysicalCores }} phys / {{ hw.cpuLogicalCores }} logical</span></div>
          <div class="row"><span class="k">{{ t('sys_ram') }}</span><span class="v num">{{ Math.round(hw.memoryAvailableMiB/1024) }} / {{ Math.round(hw.memoryTotalMiB/1024) }} GiB avail</span></div>
          <div class="row"><span class="k">{{ t('sys_gpu') }}</span><span class="v mono">{{ hw.gpuModel }}</span></div>
          <div class="row"><span class="k">vram</span><span class="v mono">{{ hw.gpuMemory }}</span></div>
        </div>
      </div>
    </div>

    <!-- Benchmark section -->
    <div class="bench-section">
      <div class="bench-head">
        <span class="panel-title">engine benchmark</span>
        <div class="bench-controls">
          <div class="ctrl">
            <label>size</label>
            <input type="number" v-model.number="benchSize" min="128" max="1024" step="128" />
          </div>
          <div class="ctrl">
            <label>iter</label>
            <input type="number" v-model.number="benchIters" min="100" max="100000" step="500" />
          </div>
          <button @click="runBenchmark" :disabled="benchRunning">
            {{ benchRunning ? 'running…' : 'run benchmark' }}
          </button>
        </div>
      </div>

      <div v-if="benchError" class="bench-error mono">{{ benchError }}</div>

      <div v-if="benchResults.length" class="bench-table">
        <div class="bench-row bench-header">
          <span class="col-engine">engine</span>
          <span class="col-scalar">scalar</span>
          <span class="col-ms">ms</span>
          <span class="col-mpps">Mpix/s</span>
          <span class="col-bar"></span>
        </div>
        <div v-for="r in benchResults" :key="r.engine + r.scalar"
             class="bench-row" :class="{ unavail: !r.available }">
          <span class="col-engine mono">{{ r.engine }}</span>
          <span class="col-scalar mono">{{ r.scalar }}</span>
          <span class="col-ms num">{{ r.available ? r.elapsedMs.toFixed(1) : '—' }}</span>
          <span class="col-mpps num accent">{{ r.available ? r.mpixPerSec.toFixed(2) : '—' }}</span>
          <span class="col-bar">
            <span v-if="r.available" class="bar-fill" :style="{ width: speedBar(r.mpixPerSec) }"></span>
          </span>
        </div>
      </div>

      <div v-else-if="!benchRunning" class="bench-hint mono">
        click "run benchmark" to compare engine × scalar throughput
      </div>
    </div>
  </div>
</template>

<style scoped>
.wrap { padding: 22px; height: 100%; overflow: auto; }

.head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 18px;
}

.grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(340px, 1fr));
  gap: 14px;
  margin-bottom: 24px;
}

.panel { padding: 16px 18px; }

.row {
  display: grid;
  grid-template-columns: 90px 1fr;
  padding: 4px 0;
  font-size: var(--fs-mono);
  align-items: baseline;
}

.k {
  color: var(--text-faint);
  font-family: var(--mono);
  font-size: var(--fs-label);
  text-transform: uppercase;
  letter-spacing: 0.08em;
}

.v { color: var(--text); }
.mono { font-family: var(--mono); font-size: var(--fs-mono); }
.num { font-variant-numeric: tabular-nums; font-family: var(--mono); }
.dim { color: var(--text-dim); }
.good { color: var(--good); }
.bad  { color: var(--bad); }

/* ---- Benchmark ---- */
.bench-section {
  margin-top: 8px;
}

.bench-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 14px;
  flex-wrap: wrap;
  gap: 10px;
}

.bench-controls {
  display: flex;
  align-items: center;
  gap: 12px;
}

.ctrl {
  display: flex;
  align-items: center;
  gap: 6px;
}

.ctrl label {
  font-size: 10px;
  text-transform: uppercase;
  letter-spacing: 0.07em;
  color: var(--text-dim);
}

.ctrl input {
  width: 80px;
}

.bench-error {
  color: var(--bad);
  font-size: 11px;
  margin-bottom: 8px;
}

.bench-hint {
  color: var(--text-faint);
  font-size: 10px;
  text-transform: uppercase;
  letter-spacing: 0.08em;
  padding: 20px 0;
}

.bench-table {
  border: 1px solid var(--rule);
}

.bench-row {
  display: grid;
  grid-template-columns: 90px 60px 70px 80px 1fr;
  padding: 6px 10px;
  align-items: center;
  border-bottom: 1px solid var(--rule);
  font-size: 12px;
}

.bench-row:last-child { border-bottom: none; }

.bench-header {
  background: var(--panel);
  font-size: 10px;
  text-transform: uppercase;
  letter-spacing: 0.07em;
  color: var(--text-dim);
}

.unavail { opacity: 0.4; }

.col-engine { font-family: var(--mono); color: var(--text); }
.col-scalar { font-family: var(--mono); color: var(--text-dim); }
.col-ms     { font-family: var(--mono); color: var(--text-dim); text-align: right; }
.col-mpps   { font-family: var(--mono); text-align: right; }
.col-bar    { padding-left: 12px; position: relative; }

.accent { color: var(--accent); }

.bar-fill {
  display: inline-block;
  height: 6px;
  background: var(--accent);
  opacity: 0.7;
  transition: width 0.3s ease;
}

@media (max-width: 760px), ((pointer: coarse) and (max-width: 1200px)), ((any-pointer: coarse) and (max-width: 1200px)), ((min-width: 761px) and (max-width: 1200px) and (orientation: landscape)) {
  .wrap {
    padding: 12px;
  }

  .head {
    gap: 10px;
    align-items: stretch;
  }

  .grid {
    grid-template-columns: 1fr;
    gap: 10px;
  }

  .panel {
    padding: 12px;
  }

  .row {
    grid-template-columns: 72px minmax(0, 1fr);
  }

  .v {
    overflow-wrap: anywhere;
  }

  .bench-controls {
    width: 100%;
    flex-wrap: wrap;
    align-items: flex-end;
  }

  .ctrl {
    flex: 1 1 120px;
  }

  .ctrl input {
    width: 100%;
  }

  .bench-controls button {
    flex: 1 1 160px;
  }

  .bench-table {
    overflow-x: auto;
  }

  .bench-row {
    min-width: 520px;
    grid-template-columns: 82px 58px 60px 72px 1fr;
    padding: 7px 8px;
  }
}











</style>
