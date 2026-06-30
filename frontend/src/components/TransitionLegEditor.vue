<script setup lang="ts">
import { computed } from 'vue'
import { VARIANTS, VARIANT_LABELS, type TransitionLegInput } from '../api'
import { lang } from '../i18n'

const AXIS_TRANSITION_VARIANTS = VARIANTS.slice(0, 10)

const props = withDefaults(defineProps<{
  modelValue: TransitionLegInput[]
  min?: number
  max?: number
}>(), {
  min: 1,
  max: 4,
})

const emit = defineEmits<{
  (e: 'update:modelValue', value: TransitionLegInput[]): void
}>()

const legs = computed(() => props.modelValue.length ? props.modelValue : [
  { variant: 'mandelbrot', weight: 1 },
])

function cleanWeight(value: number): number {
  return Number.isFinite(value) ? Math.max(0, Math.min(1, value)) : 0
}

function updateLeg(index: number, patch: Partial<TransitionLegInput>) {
  const next = legs.value.map(leg => ({ ...leg, weight: cleanWeight(leg.weight) }))
  next[index] = { ...next[index], ...patch }
  next[index].weight = cleanWeight(next[index].weight)
  emit('update:modelValue', next)
}

function addLeg() {
  if (legs.value.length >= props.max) return
  const used = new Set(legs.value.map(leg => leg.variant))
  const variant = AXIS_TRANSITION_VARIANTS.find(v => !used.has(v)) ?? AXIS_TRANSITION_VARIANTS[0]
  emit('update:modelValue', [...legs.value, { variant, weight: 1 }])
}

function removeLeg(index: number) {
  if (legs.value.length <= props.min) return
  emit('update:modelValue', legs.value.filter((_, i) => i !== index))
}

const canAdd = computed(() => legs.value.length < props.max)
</script>

<template>
  <div class="transition-leg-editor">
    <div v-for="(leg, index) in legs" :key="index" class="leg-row">
      <select
        :value="leg.variant"
        @change="updateLeg(index, { variant: ($event.target as HTMLSelectElement).value })">
        <option v-for="v in AXIS_TRANSITION_VARIANTS" :key="v" :value="v">
          {{ VARIANT_LABELS[v][lang] }}
        </option>
      </select>
      <input
        type="range"
        min="0"
        max="1"
        step="0.01"
        :value="cleanWeight(leg.weight)"
        @input="updateLeg(index, { weight: Number(($event.target as HTMLInputElement).value) })" />
      <input
        class="leg-weight num"
        type="number"
        min="0"
        max="1"
        step="0.01"
        :value="cleanWeight(leg.weight).toFixed(2)"
        @change="updateLeg(index, { weight: Number(($event.target as HTMLInputElement).value) })" />
      <button
        class="leg-remove"
        :disabled="legs.length <= min"
        title="Remove"
        @click="removeLeg(index)">
        -
      </button>
    </div>
    <button class="leg-add" :disabled="!canAdd" title="Add axis" @click="addLeg">+</button>
  </div>
</template>

<style scoped>
.transition-leg-editor {
  display: flex;
  flex-direction: column;
  gap: 6px;
}

.leg-row {
  display: grid;
  grid-template-columns: minmax(0, 1.35fr) minmax(52px, 0.9fr) 54px 28px;
  gap: 5px;
  align-items: center;
}

.leg-row select,
.leg-row input {
  min-width: 0;
}

.leg-weight {
  padding-left: 5px;
  padding-right: 5px;
  text-align: right;
}

.leg-remove,
.leg-add {
  min-width: 28px;
  height: 28px;
  padding: 0;
  line-height: 1;
}

.leg-add {
  width: 100%;
}
</style>
