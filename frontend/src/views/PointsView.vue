<script setup lang="ts">
import SpecialPointList from '../components/SpecialPointList.vue'
import { useRouter } from 'vue-router'
import type { SpecialPointEnumResult } from '../api'

const router = useRouter()

function onImportPoint(pt: SpecialPointEnumResult) {
  sessionStorage.setItem('fs_pending_center', JSON.stringify({
    re: pt.re,
    im: pt.im,
  }))
  router.push('/')
}
</script>

<template>
  <div class="wrap">
    <div class="col wide">
      <SpecialPointList @import-point="onImportPoint" />
    </div>
  </div>
</template>

<style scoped>
.wrap {
  display: block;
  gap: 1px;
  height: 100%;
  overflow: hidden;
}

.col {
  padding: 18px;
  overflow: auto;
  display: flex;
  flex-direction: column;
  gap: 16px;
}

.col.wide { background: var(--bg-raised); height: 100%; }

@media (max-width: 760px), ((pointer: coarse) and (max-width: 1200px)), ((any-pointer: coarse) and (max-width: 1200px)) {
  .col {
    padding: 12px;
  }
}
</style>
