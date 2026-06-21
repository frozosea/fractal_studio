<script setup lang="ts">
import { onMounted, onBeforeUnmount, ref, watch } from 'vue'
import { api, type MapRenderRequest, type Metric, type ColorMap, type SpecialPointEnumResult } from '../api'
import { promptSlowRenderWarning, slowRenderWarningsDisabled } from '../slowWarnings'
import { lang } from '../i18n'

// MapCanvas renders the fractal by requesting a full frame from the backend at
// the device-pixel backing-store dimensions. CSS pixels stay in charge of
// interaction math; physical pixels keep HiDPI displays from looking blocky.

const props = defineProps<{
  centerRe: number
  centerIm: number
  scale: number
  iterations: number
  variant: string   // Variant literal or "custom:HASH"
  metric: Metric
  colorMap: ColorMap
  smooth?: boolean
  colorMode?: 'direct' | 'eq_full' | 'eq_center'
  cyclesPerOctave?: number
  pairwiseCap?: number
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
  (e: 'rendered', meta: { generatedMs: number; artifactId?: string; engineUsed?: string; scalarUsed?: string }): void
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
const frameW   = ref(0)
const frameH   = ref(0)
const activeCanvas = ref(0)
const hasFrame = ref(false)
let   ro: ResizeObserver | null = null
let   dprMedia: MediaQueryList | null = null
let   renderTimer: ReturnType<typeof setTimeout> | null = null
let   currentRender: AbortController | null = null
let   renderSeq = 0
let   slowRenderWarnKey = ''
const renderClientId = `map-${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`
const MIN_FRAME_DIM = 64
const MAX_FRAME_DIM = 4096

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

function devicePixelRatioSafe(): number {
  const dpr = window.devicePixelRatio
  return Number.isFinite(dpr) && dpr > 0 ? dpr : 1
}

function clampFrameSize(cssW: number, cssH: number, targetScale: number): { width: number; height: number } {
  if (cssW <= 0 || cssH <= 0) return { width: 0, height: 0 }
  const desiredScale = Number.isFinite(targetScale) && targetScale > 0 ? targetScale : 1
  const maxScale = Math.min(MAX_FRAME_DIM / cssW, MAX_FRAME_DIM / cssH)
  const scale = Math.min(desiredScale, maxScale)
  return {
    width: Math.max(1, Math.round(cssW * scale)),
    height: Math.max(1, Math.round(cssH * scale)),
  }
}

function devicePixelBoxSize(entry: ResizeObserverEntry): { width: number; height: number } | null {
  const box = entry.devicePixelContentBoxSize?.[0]
  if (!box || box.inlineSize <= 0 || box.blockSize <= 0) return null
  return {
    width: Math.round(box.inlineSize),
    height: Math.round(box.blockSize),
  }
}

function renderableSize(): boolean {
  return frameW.value >= MIN_FRAME_DIM && frameH.value >= MIN_FRAME_DIM
}

function notifyPreempt(seq: number) {
  api.mapPreempt({ preemptKey: renderClientId, preemptSeq: seq }).catch(() => {})
}

function maybeWarnSlowRender(generatedMs: number) {
  if (!(generatedMs > 1000)) return
  if (slowRenderWarningsDisabled()) return
  const key = [
    props.metric,
    props.iterations,
    props.pairwiseCap ?? 64,
    frameW.value,
    frameH.value,
    props.engine ?? 'auto',
    props.scalarType ?? 'auto',
  ].join(':')
  if (slowRenderWarnKey === key) return
  slowRenderWarnKey = key
  promptSlowRenderWarning(lang.value === 'en'
    ? `Render took ${(generatedMs / 1000).toFixed(2)}s. Consider lowering resolution / iterations / pairwise cap, or keep it for an offline-quality pass.`
    : `渲染耗时 ${(generatedMs / 1000).toFixed(2)} 秒。可降低分辨率/迭代次数/成对距离上限，或保留此设置用于离线高质量渲染。`)
}

function setViewportSize(cssW: number, cssH: number, physical?: { width: number; height: number } | null) {
  const targetScale = physical && cssW > 0 && cssH > 0
    ? Math.min(physical.width / cssW, physical.height / cssH)
    : devicePixelRatioSafe()
  const nextFrame = clampFrameSize(cssW, cssH, targetScale)
  const cssChanged = cssW !== domW.value || cssH !== domH.value
  const frameChanged = nextFrame.width !== frameW.value || nextFrame.height !== frameH.value
  if (!cssChanged && !frameChanged) return

  domW.value = cssW
  domH.value = cssH
  frameW.value = nextFrame.width
  frameH.value = nextFrame.height
  if (cssChanged) emit('viewport-size', { width: cssW, height: cssH })
  if (!hasFrame.value) {
    resizeCanvas(canvasA.value, nextFrame.width, nextFrame.height)
    resizeCanvas(canvasB.value, nextFrame.width, nextFrame.height)
  }
}

function updateViewportSizeFromWrapper() {
  if (!wrapper.value) return
  const rect = wrapper.value.getBoundingClientRect()
  setViewportSize(Math.round(rect.width), Math.round(rect.height))
}

function invalidateCurrentRender() {
  const hadRender = currentRender !== null
  currentRender?.abort()
  currentRender = null
  renderSeq += 1
  if (hadRender) notifyPreempt(renderSeq)
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

function drawRgbaFrame(data: ArrayBuffer, width: number, height: number): number | null {
  const expectedBytes = width * height * 4
  if (data.byteLength !== expectedBytes) {
    throw new Error(`inline frame size mismatch: got ${data.byteLength}, expected ${expectedBytes}`)
  }
  const next = activeCanvas.value === 0 ? 1 : 0
  const target = canvasByIndex(next)
  resizeCanvas(target, width, height)
  const ctx = target?.getContext('2d')
  if (!ctx) return null
  const imageData = new ImageData(new Uint8ClampedArray(data), width, height)
  ctx.putImageData(imageData, 0, 0)
  return next
}

async function renderFrame() {
  if (!renderableSize()) return
  pending.value = true
  error.value   = ''

  // Abort any in-flight render
  currentRender?.abort()
  const controller = new AbortController()
  currentRender = controller
  const seq = ++renderSeq
  const requestId = `${renderClientId}-${seq}`
  const reqW = frameW.value
  const reqH = frameH.value

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
    colorMode:  props.colorMode,
    cyclesPerOctave: props.cyclesPerOctave,
    pairwiseCap: props.pairwiseCap,
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
  if (props.transitionFrom)           req.transitionFrom           = props.transitionFrom
  if (props.transitionTo)             req.transitionTo             = props.transitionTo
  if (props.engine)                   req.engine                    = props.engine
  if (props.scalarType)               req.scalarType                = props.scalarType

  try {
    const resp = await api.mapRenderInline(req, controller.signal) as any
    if (seq !== renderSeq || (resp.requestId && resp.requestId !== requestId)) return
    if (resp.status === 'cancelled' || !resp.data) return
    if (resp.pixelFormat && resp.pixelFormat !== 'rgba8') {
      throw new Error(`unsupported inline frame format: ${resp.pixelFormat}`)
    }
    const frameW = resp.width || reqW
    const frameH = resp.height || reqH
    const next = drawRgbaFrame(resp.data, frameW, frameH)
    if (next === null) return
    if (seq !== renderSeq) return
    renderedViewport = {
      centerRe: req.centerRe,
      centerIm: req.centerIm,
      scale: req.scale,
    }
    hasFrame.value = true
    activeCanvas.value = next
    maybeWarnSlowRender(resp.generatedMs)
    emit('rendered', {
      generatedMs: resp.generatedMs,
      artifactId:  '',
      engineUsed:  resp.engineUsed,
      scalarUsed:  resp.scalarUsed,
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
  if (renderableSize()) {
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
  props.colorMode, props.cyclesPerOctave,
  props.iterations, props.pairwiseCap, props.julia, props.juliaRe, props.juliaIm,
  props.engine, props.scalarType, props.transitionTheta, props.transitionThetaMilliDeg,
  props.transitionFrom, props.transitionTo,
  frameW.value, frameH.value,
], () => scheduleRender())

// ── Lifecycle ─────────────────────────────────────────────────────────────────

onMounted(() => {
  if (!wrapper.value || !canvasA.value || !canvasB.value) return
  ro = new ResizeObserver(entries => {
    for (const e of entries) {
      const w = Math.round(e.contentRect.width)
      const h = Math.round(e.contentRect.height)
      setViewportSize(w, h, devicePixelBoxSize(e))
    }
  })
  ro.observe(wrapper.value)
  updateViewportSizeFromWrapper()
  resizeCanvas(canvasA.value, frameW.value, frameH.value)
  resizeCanvas(canvasB.value, frameW.value, frameH.value)
  window.addEventListener('resize', updateViewportSizeFromWrapper)
  watchDevicePixelRatio()
  scheduleRender(0)
})

onBeforeUnmount(() => {
  ro?.disconnect()
  window.removeEventListener('resize', updateViewportSizeFromWrapper)
  unwatchDevicePixelRatio()
  if (renderTimer) clearTimeout(renderTimer)
  invalidateCurrentRender()
})

function onDevicePixelRatioChange() {
  updateViewportSizeFromWrapper()
  watchDevicePixelRatio()
}

function watchDevicePixelRatio() {
  unwatchDevicePixelRatio()
  dprMedia = window.matchMedia(`(resolution: ${devicePixelRatioSafe()}dppx)`)
  dprMedia.addEventListener('change', onDevicePixelRatioChange, { once: true })
}

function unwatchDevicePixelRatio() {
  dprMedia?.removeEventListener('change', onDevicePixelRatioChange)
  dprMedia = null
}

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

type PointerPoint = { x: number; y: number }
type PinchStart = {
  distance: number
  scale: number
  worldRe: number
  worldIm: number
}

const activePointers = new Map<number, PointerPoint>()
let dragging  = false
let dragMoved = false
let dragStart = { x: 0, y: 0, cx: 0, cy: 0, sc: 0 }
let pinchStart: PinchStart | null = null

function screenToWorld(clientX: number, clientY: number): { re: number; im: number } | null {
  if (!wrapper.value) return null
  const rect   = wrapper.value.getBoundingClientRect()
  const aspect = rect.width / rect.height
  const px     = (clientX - rect.left) / rect.width
  const py     = (clientY - rect.top)  / rect.height
  return {
    re: props.centerRe + (px - 0.5) * props.scale * aspect,
    im: props.centerIm - (py - 0.5) * props.scale,
  }
}

function pointerValues(): PointerPoint[] {
  return [...activePointers.values()]
}

function pointerDistance(points: PointerPoint[]): number {
  if (points.length < 2) return 0
  return Math.hypot(points[0].x - points[1].x, points[0].y - points[1].y)
}

function pointerMidpoint(points: PointerPoint[]): PointerPoint {
  if (points.length < 2) return points[0] ?? { x: 0, y: 0 }
  return {
    x: (points[0].x + points[1].x) * 0.5,
    y: (points[0].y + points[1].y) * 0.5,
  }
}

function startPinch() {
  const points = pointerValues()
  if (points.length < 2) {
    pinchStart = null
    return
  }
  const midpoint = pointerMidpoint(points)
  const world = screenToWorld(midpoint.x, midpoint.y)
  const distance = pointerDistance(points)
  if (!world || distance <= 0) {
    pinchStart = null
    return
  }
  pinchStart = {
    distance,
    scale: props.scale,
    worldRe: world.re,
    worldIm: world.im,
  }
}

function updatePinch() {
  if (!wrapper.value || !pinchStart) return
  const points = pointerValues()
  if (points.length < 2) return
  const distance = pointerDistance(points)
  if (distance <= 0) return

  const rect = wrapper.value.getBoundingClientRect()
  const midpoint = pointerMidpoint(points)
  const px = (midpoint.x - rect.left) / rect.width
  const py = (midpoint.y - rect.top) / rect.height
  const aspect = rect.width / rect.height
  const newScale = Math.max(1e-300, pinchStart.scale * (pinchStart.distance / distance))
  emit('viewport-change', {
    centerRe: pinchStart.worldRe - (px - 0.5) * newScale * aspect,
    centerIm: pinchStart.worldIm + (py - 0.5) * newScale,
    scale: newScale,
  })
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
  const prefix = (p.fallback || !p.accepted) ? (lang.value === 'en' ? 'fallback ' : '回退 ') : ''
  return `${prefix}${p.kind} p${p.period} ${p.re.toPrecision(8)} ${p.im.toPrecision(8)}i`
}

function onPointerDown(e: PointerEvent) {
  activePointers.set(e.pointerId, { x: e.clientX, y: e.clientY })
  ;(e.currentTarget as HTMLElement).setPointerCapture(e.pointerId)
  if (activePointers.size >= 2) {
    dragging = false
    dragMoved = true
    startPinch()
    return
  }
  dragging  = true
  dragMoved = false
  dragStart = { x: e.clientX, y: e.clientY, cx: props.centerRe, cy: props.centerIm, sc: props.scale }
}

function onPointerMove(e: PointerEvent) {
  if (!activePointers.has(e.pointerId)) return
  activePointers.set(e.pointerId, { x: e.clientX, y: e.clientY })
  if (activePointers.size >= 2) {
    dragMoved = true
    updatePinch()
    return
  }
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

function onPointerUp(e: PointerEvent) {
  const wasTap = dragging && !dragMoved && activePointers.size === 1 && activePointers.has(e.pointerId)
  if (wasTap) {
    const w = screenToWorld(e.clientX, e.clientY)
    if (w) emit('click-world', w)
  }
  activePointers.delete(e.pointerId)
  const target = e.currentTarget as HTMLElement
  if (target.hasPointerCapture?.(e.pointerId)) target.releasePointerCapture(e.pointerId)

  if (activePointers.size === 1) {
    const next = pointerValues()[0]
    dragging = true
    dragMoved = false
    dragStart = { x: next.x, y: next.y, cx: props.centerRe, cy: props.centerIm, sc: props.scale }
  } else {
    dragging = false
    pinchStart = null
  }
}

function onPointerCancel(e: PointerEvent) {
  activePointers.delete(e.pointerId)
  if (activePointers.size < 2) pinchStart = null
  if (activePointers.size === 0) dragging = false
}
</script>

<template>
  <div class="map-wrap"
       ref="wrapper"
       @wheel.prevent="onWheel"
       @pointerdown="onPointerDown"
       @pointermove="onPointerMove"
       @pointerup="onPointerUp"
       @pointercancel="onPointerCancel"
       @lostpointercapture="onPointerCancel">
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
        @pointerdown.stop
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
  touch-action: none;
  overflow: hidden;
}

.map-wrap:active { cursor: grabbing; }

.frame {
  position: absolute;
  inset: 0;
  display: block;
  width: 100%;
  height: 100%;
  image-rendering: auto;
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
