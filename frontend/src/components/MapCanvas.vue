<script setup lang="ts">
import { onMounted, onBeforeUnmount, ref, watch } from 'vue'
import { api, type MapRenderRequest, type Metric, type ColorMap, type SpecialPointEnumResult } from '../api'

// MapCanvas renders the fractal by requesting a full frame from the backend at
// the exact canvas pixel dimensions. Every param/pan/zoom change triggers a
// debounced full re-render; the previous frame remains visible while loading.

const props = defineProps<{
  centerRe: number
  centerIm: number
  scale: number
  iterations: number
  variant: string   // Variant literal or "custom:HASH"
  metric: Metric
  colorMap: ColorMap
  smooth?: boolean
  transitionTheta: number | null
  transitionThetaMilliDeg?: number | null
  transitionFrom?: string
  transitionTo?: string
  julia?: boolean
  juliaRe?: number
  juliaIm?: number
  engine?: string
  scalarType?: string
  specialPoints?: SpecialPointEnumResult[]
  hoveredSpecialPointId?: string
  selectedSpecialPointId?: string
}>()

const emit = defineEmits<{
  (e: 'viewport-change', v: { centerRe: number; centerIm: number; scale: number }): void
  (e: 'viewport-size', size: { width: number; height: number }): void
  (e: 'rendered', meta: { generatedMs: number; artifactId: string; engineUsed?: string; scalarUsed?: string }): void
  (e: 'click-world', pos: { re: number; im: number }): void
  (e: 'hover-special-point', id: string): void
  (e: 'select-special-point', p: SpecialPointEnumResult): void
}>()

// ── DOM refs ──────────────────────────────────────────────────────────────────
const wrapper  = ref<HTMLDivElement | null>(null)
const canvasA = ref<HTMLCanvasElement | null>(null)
const canvasB = ref<HTMLCanvasElement | null>(null)
const pending  = ref(false)
const error    = ref('')
const domW     = ref(0)
const domH     = ref(0)
const activeCanvas = ref(0)
const hasFrame = ref(false)
let   ro: ResizeObserver | null = null
let   renderTimer: ReturnType<typeof setTimeout> | null = null
let   currentRender: AbortController | null = null
let   renderSeq = 0
const renderClientId = `map-${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`

type ViewportSnapshot = {
  centerRe: number
  centerIm: number
  scale: number
}

let renderedViewport: ViewportSnapshot | null = null

// ── Full-frame render ─────────────────────────────────────────────────────────

function canvasByIndex(index: number): HTMLCanvasElement | null {
  return index === 0 ? canvasA.value : canvasB.value
}

function resizeCanvas(c: HTMLCanvasElement | null, w: number, h: number) {
  if (!c) return
  if (c.width !== w) c.width = w
  if (c.height !== h) c.height = h
}

function notifyPreempt(seq: number) {
  api.mapPreempt({ preemptKey: renderClientId, preemptSeq: seq }).catch(() => {})
}

function setDomSize(w: number, h: number) {
  if (w === domW.value && h === domH.value) return
  domW.value = w
  domH.value = h
  emit('viewport-size', { width: w, height: h })
  if (!hasFrame.value) {
    resizeCanvas(canvasA.value, w, h)
    resizeCanvas(canvasB.value, w, h)
  }
}

function invalidateCurrentRender() {
  currentRender?.abort()
  currentRender = null
  renderSeq += 1
  notifyPreempt(renderSeq)
}

function previewTransform(): string {
  if (!hasFrame.value || !renderedViewport || domW.value < 16 || domH.value < 16) return 'none'
  const aspect = domW.value / domH.value
  const scaleRatio = renderedViewport.scale / props.scale
  const dx = (renderedViewport.centerRe - props.centerRe) * domW.value / (props.scale * aspect)
  const dy = -(renderedViewport.centerIm - props.centerIm) * domH.value / props.scale
  const tx = domW.value * (1 - scaleRatio) * 0.5 + dx
  const ty = domH.value * (1 - scaleRatio) * 0.5 + dy
  if (Math.abs(scaleRatio - 1) < 0.000001 && Math.abs(tx) < 0.01 && Math.abs(ty) < 0.01) return 'none'
  return `matrix(${scaleRatio}, 0, 0, ${scaleRatio}, ${tx}, ${ty})`
}

function canvasStyle(index: number) {
  return {
    opacity: activeCanvas.value === index ? 1 : 0,
    transform: activeCanvas.value === index ? previewTransform() : 'none',
  }
}

async function renderFrame() {
  if (domW.value < 16 || domH.value < 16) return
  pending.value = true
  error.value   = ''

  // Abort any in-flight render
  currentRender?.abort()
  const controller = new AbortController()
  currentRender = controller
  const seq = ++renderSeq
  const requestId = `${renderClientId}-${seq}`
  const reqW = domW.value
  const reqH = domH.value

  const req: MapRenderRequest = {
    requestId,
    preemptKey: renderClientId,
    preemptSeq: seq,
    centerRe:   props.centerRe,
    centerIm:   props.centerIm,
    scale:      props.scale,
    width:      reqW,
    height:     reqH,
    iterations: props.iterations,
    variant:    props.variant,
    metric:     props.metric,
    colorMap:   props.colorMap,
    smooth:     props.smooth,
    julia:      props.julia,
    juliaRe:    props.juliaRe ?? 0,
    juliaIm:    props.juliaIm ?? 0,
  }
  if (props.transitionThetaMilliDeg !== null && props.transitionThetaMilliDeg !== undefined) {
    req.transitionThetaMilliDeg = props.transitionThetaMilliDeg
    req.transitionTheta = props.transitionThetaMilliDeg * Math.PI / 180000
  } else if (props.transitionTheta !== null) {
    req.transitionTheta = props.transitionTheta
  }
  if (props.transitionFrom)           (req as any).transitionFrom  = props.transitionFrom
  if (props.transitionTo)             (req as any).transitionTo    = props.transitionTo
  if (props.engine)                   (req as any).engine           = props.engine
  if (props.scalarType)               (req as any).scalarType       = props.scalarType

  try {
    const resp = await api.mapRender(req, controller.signal) as any
    if (seq !== renderSeq || (resp.requestId && resp.requestId !== requestId)) return
    if (resp.status === 'cancelled' || !resp.artifactId) return
    const imgUrl = api.artifactContentUrl(resp.artifactId)
    await new Promise<void>((res, rej) => {
      const img = new Image()
      img.onload = () => {
        if (seq !== renderSeq) { res(); return }
        const next = activeCanvas.value === 0 ? 1 : 0
        const target = canvasByIndex(next)
        resizeCanvas(target, reqW, reqH)
        const ctx = target?.getContext('2d')
        if (ctx) {
          ctx.clearRect(0, 0, reqW, reqH)
          ctx.drawImage(img, 0, 0, reqW, reqH)
          renderedViewport = {
            centerRe: req.centerRe,
            centerIm: req.centerIm,
            scale: req.scale,
          }
          hasFrame.value = true
          activeCanvas.value = next
        }
        res()
      }
      img.onerror = rej
      img.src = imgUrl
    })
    if (seq !== renderSeq) return
    emit('rendered', {
      generatedMs: resp.generatedMs,
      artifactId:  resp.artifactId,
      engineUsed:  resp.effective?.engine ?? resp.engineUsed,
      scalarUsed:  resp.effective?.scalar ?? resp.scalarUsed,
    })
  } catch (e: any) {
    if (seq === renderSeq && e?.name !== 'AbortError') error.value = e?.message ?? String(e)
  } finally {
    if (currentRender === controller) currentRender = null
    if (seq === renderSeq) pending.value = false
  }
}

function scheduleRender(delay = 200) {
  invalidateCurrentRender()
  if (domW.value >= 16 && domH.value >= 16) {
    pending.value = true
    error.value = ''
  }
  if (renderTimer) clearTimeout(renderTimer)
  renderTimer = setTimeout(renderFrame, delay)
}

// ── Watchers ──────────────────────────────────────────────────────────────────

watch(() => [
  props.centerRe, props.centerIm, props.scale,
  props.variant, props.metric, props.colorMap, props.smooth,
  props.iterations, props.julia, props.juliaRe, props.juliaIm,
  props.engine, props.scalarType, props.transitionTheta, props.transitionThetaMilliDeg,
  props.transitionFrom, props.transitionTo,
  domW.value, domH.value,
], () => scheduleRender())

// ── Lifecycle ─────────────────────────────────────────────────────────────────

onMounted(() => {
  if (!wrapper.value || !canvasA.value || !canvasB.value) return
  ro = new ResizeObserver(entries => {
    for (const e of entries) {
      const w = Math.round(e.contentRect.width)
      const h = Math.round(e.contentRect.height)
      setDomSize(w, h)
    }
  })
  ro.observe(wrapper.value)
  const w = Math.round(wrapper.value.clientWidth)
  const h = Math.round(wrapper.value.clientHeight)
  setDomSize(w, h)
  resizeCanvas(canvasA.value, w, h)
  resizeCanvas(canvasB.value, w, h)
  scheduleRender(0)
})

onBeforeUnmount(() => {
  ro?.disconnect()
  if (renderTimer) clearTimeout(renderTimer)
  invalidateCurrentRender()
})

// ── Interaction ───────────────────────────────────────────────────────────────

function onWheel(e: WheelEvent) {
  if (!wrapper.value) return
  const rect    = wrapper.value.getBoundingClientRect()
  const px      = (e.clientX - rect.left) / rect.width
  const py      = (e.clientY - rect.top)  / rect.height
  const aspect  = rect.width / rect.height
  const wx      = props.centerRe + (px - 0.5) * props.scale * aspect
  const wy      = props.centerIm + (0.5 - py) * props.scale
  const factor  = e.deltaY > 0 ? 1.25 : 0.8
  const newScale = props.scale * factor
  emit('viewport-change', {
    centerRe: wx - (px - 0.5) * newScale * aspect,
    centerIm: wy + (py - 0.5) * newScale,
    scale:    newScale,
  })
}

let dragging  = false
let dragMoved = false
let dragStart = { x: 0, y: 0, cx: 0, cy: 0, sc: 0 }

function screenToWorld(e: MouseEvent): { re: number; im: number } | null {
  if (!wrapper.value) return null
  const rect   = wrapper.value.getBoundingClientRect()
  const aspect = rect.width / rect.height
  const px     = (e.clientX - rect.left) / rect.width
  const py     = (e.clientY - rect.top)  / rect.height
  return {
    re: props.centerRe + (px - 0.5) * props.scale * aspect,
    im: props.centerIm - (py - 0.5) * props.scale,
  }
}

function worldToScreen(re: number, im: number): { x: number; y: number; visible: boolean } {
  const w = Math.max(1, domW.value)
  const h = Math.max(1, domH.value)
  const aspect = w / h
  const x = (0.5 + (re - props.centerRe) / (props.scale * aspect)) * w
  const y = (0.5 - (im - props.centerIm) / props.scale) * h
  return { x, y, visible: x >= -8 && x <= w + 8 && y >= -8 && y <= h + 8 }
}

function markerStyle(p: SpecialPointEnumResult) {
  const pos = worldToScreen(p.re, p.im)
  return {
    transform: `translate(${pos.x}px, ${pos.y}px)`,
    display: pos.visible ? 'block' : 'none',
  }
}

function markerTitle(p: SpecialPointEnumResult) {
  const prefix = (p.fallback || !p.accepted) ? 'fallback ' : ''
  return `${prefix}${p.kind} p${p.period} ${p.re.toPrecision(8)} ${p.im.toPrecision(8)}i`
}

function onMouseDown(e: MouseEvent) {
  dragging  = true
  dragMoved = false
  dragStart = { x: e.clientX, y: e.clientY, cx: props.centerRe, cy: props.centerIm, sc: props.scale }
}

function onMouseMove(e: MouseEvent) {
  if (!dragging || !wrapper.value) return
  const dx = e.clientX - dragStart.x
  const dy = e.clientY - dragStart.y
  if (!dragMoved && Math.hypot(dx, dy) < 5) return
  dragMoved = true
  const rect   = wrapper.value.getBoundingClientRect()
  const aspect = rect.width / rect.height
  emit('viewport-change', {
    centerRe: dragStart.cx - (dx / rect.width)  * dragStart.sc * aspect,
    centerIm: dragStart.cy + (dy / rect.height) * dragStart.sc,
    scale:    dragStart.sc,
  })
}

function onMouseUp(e: MouseEvent) {
  if (dragging && !dragMoved) {
    const w = screenToWorld(e)
    if (w) emit('click-world', w)
  }
  dragging = false
}
</script>

<template>
  <div class="map-wrap"
       ref="wrapper"
       @wheel.prevent="onWheel"
       @mousedown="onMouseDown"
       @mousemove="onMouseMove"
       @mouseup="onMouseUp"
       @mouseleave="onMouseUp">
    <canvas
      ref="canvasA"
      class="frame"
      :class="{ active: activeCanvas === 0, stale: pending && activeCanvas === 0 }"
      :style="canvasStyle(0)"
    />
    <canvas
      ref="canvasB"
      class="frame"
      :class="{ active: activeCanvas === 1, stale: pending && activeCanvas === 1 }"
      :style="canvasStyle(1)"
    />
    <div v-if="pending" class="busy-line"></div>
    <div v-if="specialPoints?.length" class="markers">
      <button
        v-for="p in specialPoints"
        :key="p.id"
        class="marker"
        :class="{ hover: hoveredSpecialPointId === p.id, selected: selectedSpecialPointId === p.id, misi: p.kind === 'misiurewicz', fallback: p.fallback || !p.accepted }"
        :style="markerStyle(p)"
        :title="markerTitle(p)"
        @mousedown.stop
        @mouseenter="$emit('hover-special-point', p.id)"
        @mouseleave="$emit('hover-special-point', '')"
        @click.stop="$emit('select-special-point', p)">
      </button>
    </div>
    <div v-if="error"   class="overlay error">{{ error }}</div>
  </div>
</template>

<style scoped>
.map-wrap {
  position: relative;
  width: 100%;
  height: 100%;
  background: var(--bg, #0a0b0d);
  cursor: grab;
  user-select: none;
  overflow: hidden;
}

.map-wrap:active { cursor: grabbing; }

.frame {
  position: absolute;
  inset: 0;
  display: block;
  width: 100%;
  height: 100%;
  image-rendering: pixelated;
  opacity: 0;
  transform-origin: 0 0;
  transition:
    opacity 160ms ease,
    transform 140ms ease,
    filter 140ms ease;
  will-change: opacity, transform, filter;
}

.frame.active {
  opacity: 1;
}

.frame.stale {
  filter: brightness(0.82) saturate(0.9);
}

.busy-line {
  position: absolute;
  left: 0;
  right: 0;
  top: 0;
  height: 2px;
  overflow: hidden;
  pointer-events: none;
}

.busy-line::before {
  content: '';
  display: block;
  width: 42%;
  height: 100%;
  background: var(--accent);
  opacity: 0.75;
  animation: sweep 900ms ease-in-out infinite;
}

@keyframes sweep {
  0% { transform: translateX(-110%); }
  100% { transform: translateX(250%); }
}

.overlay {
  position: absolute;
  left: 50%; top: 8px;
  transform: translateX(-50%);
  background: var(--panel);
  border: 1px solid var(--rule);
  padding: 4px 10px;
  font-family: var(--mono);
  font-size: var(--fs-label);
  text-transform: uppercase;
  letter-spacing: 0.1em;
  color: var(--text-dim);
  pointer-events: none;
}

.overlay.error {
  color: var(--bad);
  border-color: var(--bad);
}

.markers {
  position: absolute;
  inset: 0;
  pointer-events: none;
}

.marker {
  position: absolute;
  left: 0;
  top: 0;
  width: 11px;
  height: 11px;
  min-width: 11px;
  padding: 0;
  margin: -5.5px 0 0 -5.5px;
  border-radius: 50%;
  border: 1px solid #fff7a8;
  background: rgba(255, 210, 70, 0.35);
  box-shadow: 0 0 0 1px rgba(0,0,0,0.6), 0 0 10px rgba(255, 210, 70, 0.6);
  pointer-events: auto;
  cursor: pointer;
}

.marker.misi {
  border-color: #92d4ff;
  background: rgba(80, 170, 255, 0.35);
}

.marker.fallback {
  border-style: dashed;
  border-color: #d5ad45;
  background: rgba(213, 173, 69, 0.28);
}

.marker.hover,
.marker:hover {
  width: 15px;
  height: 15px;
  min-width: 15px;
  margin: -7.5px 0 0 -7.5px;
  border-width: 2px;
}

.marker.selected {
  width: 17px;
  height: 17px;
  min-width: 17px;
  margin: -8.5px 0 0 -8.5px;
  border-width: 2px;
  background: rgba(255, 255, 255, 0.75);
}
</style>
