<script setup lang="ts">
import { onMounted, ref } from 'vue'
import { api, type RunRow, type ArtifactRow } from '../api'
import { t } from '../i18n'

const runs = ref<RunRow[]>([])
const artifacts = ref<ArtifactRow[]>([])
const selectedRun = ref<string>('')
const loading = ref(false)

async function refresh() {
  loading.value = true
  try {
    const r = await api.runs(100)
    runs.value = r.items
    const a = await api.artifacts()
    artifacts.value = a.items
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
  return (row.finishedAt - row.startedAt) + 'ms'
}

function runArtifacts(runId: string) {
  return artifacts.value.filter(a => a.runId === runId)
}

onMounted(refresh)
</script>

<template>
  <div class="wrap">
    <div class="head">
      <span class="panel-title">{{ t('runs_title') }} ({{ runs.length }})</span>
      <button @click="refresh" :disabled="loading">{{ t('render') }}</button>
    </div>
    <table class="runs">
      <thead>
        <tr>
          <th>{{ t('runs_id') }}</th>
          <th>{{ t('runs_module') }}</th>
          <th>{{ t('runs_status') }}</th>
          <th>started</th>
          <th>took</th>
          <th>{{ t('runs_artifacts') }}</th>
        </tr>
      </thead>
      <tbody>
        <tr v-for="r in runs" :key="r.id"
            :class="{ selected: selectedRun === r.id }"
            @click="selectedRun = (selectedRun === r.id ? '' : r.id)">
          <td class="mono dim">{{ r.id }}</td>
          <td class="mono">{{ r.module }}</td>
          <td class="mono" :class="{ good: r.status === 'completed', bad: r.status === 'failed' }">
            {{ r.status }}
          </td>
          <td class="mono dim">{{ fmtTime(r.startedAt) }}</td>
          <td class="num">{{ fmtDuration(r) }}</td>
          <td class="num">{{ runArtifacts(r.id).length }}</td>
        </tr>
      </tbody>
    </table>

    <div v-if="selectedRun" class="artifacts">
      <div class="panel-title">{{ t('runs_artifacts') }} for {{ selectedRun }}</div>
      <ul>
        <li v-for="a in runArtifacts(selectedRun)" :key="a.artifactId">
          <a class="mono" :href="api.artifactDownloadUrl(a.artifactId)" target="_blank">
            {{ a.name }}
          </a>
          <span class="dim"> · {{ a.kind }} · {{ (a.sizeBytes / 1024).toFixed(1) }} KiB</span>
        </li>
      </ul>
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
}

.runs {
  width: 100%;
  border-collapse: collapse;
  font-size: var(--fs-mono);
  font-family: var(--mono);
}

.runs th {
  font-size: var(--fs-label);
  color: var(--text-faint);
  text-transform: uppercase;
  letter-spacing: 0.08em;
  text-align: left;
  padding: 6px 10px;
  border-bottom: 1px solid var(--rule);
  font-weight: normal;
}

.runs td {
  padding: 6px 10px;
  border-bottom: 1px solid var(--rule);
  cursor: pointer;
}

.runs tr:hover td { background: var(--bg-raised); }
.runs tr.selected td { background: var(--accent-weak); }

.num { font-variant-numeric: tabular-nums; text-align: right; }
.dim { color: var(--text-dim); }
.mono { font-family: var(--mono); }

.good { color: var(--good); }
.bad  { color: var(--bad); }

.artifacts {
  margin-top: 18px;
  padding: 14px;
  border: 1px solid var(--rule);
  background: var(--panel);
}

.artifacts ul { list-style: none; }
.artifacts li { padding: 3px 0; font-size: var(--fs-mono); }
.artifacts a:hover { color: var(--accent); }

@media (max-width: 760px), ((pointer: coarse) and (max-width: 1200px)), ((any-pointer: coarse) and (max-width: 1200px)), ((min-width: 761px) and (max-width: 1200px) and (orientation: landscape)) {
  .wrap {
    padding: 12px;
  }

  .head {
    gap: 10px;
    align-items: stretch;
  }

  .runs {
    min-width: 680px;
  }

  .runs th,
  .runs td {
    padding: 7px 8px;
  }

  .artifacts {
    padding: 12px;
    overflow-wrap: anywhere;
  }
}




</style>
