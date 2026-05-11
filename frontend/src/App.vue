<script setup lang="ts">
import NavRail    from './components/NavRail.vue'
import StatusRail from './components/StatusRail.vue'
import { provide, reactive, ref } from 'vue'
import { isMobileDevice } from './device'
import type { StatusState } from './types'

const status = reactive<StatusState>({
  cpu: null,
  gpu: null,
  renderMs: null,
  engine: 'openmp',
  scalar: 'fp64',
  cRe: null,
  cIm: null,
  zoom: null,
  iter: null,
  variant: 'mandelbrot',
  metric: 'escape',
  message: 'ready',
})

provide('status', status)

const navCollapsed = ref(false)
const statusCollapsed = ref(isMobileDevice)
</script>

<template>
  <div
    class="shell"
    :class="{
      'nav-collapsed': navCollapsed,
      'status-collapsed': statusCollapsed,
    }">
    <NavRail
      :collapsed="navCollapsed"
      @toggle="navCollapsed = !navCollapsed"
    />
    <main class="main">
      <router-view />
    </main>
    <StatusRail
      :status="status"
      :collapsed="statusCollapsed"
      @toggle="statusCollapsed = !statusCollapsed"
    />
  </div>
</template>

<style scoped>
.shell {
  display: grid;
  grid-template-columns: var(--nav-current-w, var(--nav-w)) 1fr var(--rail-current-w, var(--rail-w));
  height: 100vh;
  overflow: hidden;
}

.shell.nav-collapsed {
  --nav-current-w: 28px;
}

.shell.status-collapsed {
  --rail-current-w: 28px;
}

.main {
  border-left:  1px solid var(--rule);
  border-right: 1px solid var(--rule);
  overflow: auto;
}

:global(html[data-device='mobile']) .shell {
  grid-template-columns: 1fr;
  grid-template-rows: 48px minmax(0, 1fr) auto;
  height: 100dvh;
}

:global(html[data-device='mobile']) .main {
  border-left: none;
  border-right: none;
  min-height: 0;
}

:global(html[data-device='mobile']) .shell.nav-collapsed {
  --nav-current-w: var(--nav-w);
}

:global(html[data-device='mobile']) .shell.status-collapsed {
  --rail-current-w: var(--rail-w);
}
</style>
