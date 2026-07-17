<script setup lang="ts">
import { computed, onMounted, onBeforeUnmount, ref, watch } from 'vue'
import { api, type MapRenderRequest, type MapFieldRequest, type MapFieldSessionStartRequest, type MapFieldSessionStatus, type MapFieldSessionResult, type Metric, type ColorMap, type SpecialPointEnumResult } from '../api'
import { promptSlowRenderWarning, slowRenderWarningsDisabled } from '../slowWarnings'
import { lang } from '../i18n'
import { GlColorizer, isWebGL2Available } from '../gl-colorize'

// MapCanvas renders the fractal by requesting a full frame from the backend at
// the device-pixel backing-store dimensions. CSS pixels stay in charge of
// interaction math; physical pixels keep HiDPI displays from looking blocky.

const props = defineProps<{
  centerRe: number
  centerIm: number
  centerReStr?: string
  centerImStr?: string
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
  transitionVariants?: string[]
  transitionWeights?: number[]
  julia?: boolean
  juliaRe?: number
  juliaIm?: number
  engine?: string
  scalarType?: string
  rotationDeg?: number
  // Bumped by the parent on non-incremental center jumps (typed coordinates,
  // imports, reset). Clears the offset-space interaction state below.
  viewEpoch?: number
  showExportFrame?: boolean
  exportFrameWidth?: number
  exportFrameHeight?: number
  specialPoints?: SpecialPointEnumResult[]
  hoveredSpecialPointId?: string
  selectedSpecialPointId?: string
}>()

const emit = defineEmits<{
  (e: 'viewport-change', v: { centerRe: number; centerIm: number; scale: number; deltaRe?: number; deltaIm?: number }): void
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
// Once a slow native session has supplied a usable snapshot, it is the live
// interaction frame rather than a stale placeholder.  Keeping this separate
// from `pending` lets the backend continue refining without dimming or
// repeatedly fading the preview.
const interactivePreviewVisible = ref(false)
// The previous canvas remains a stable transformed interaction preview until
// the backend itself says this native field missed its latency budget. This
// prevents an immediate dim/bright flash for renders that complete quickly.
const interactiveSessionPending = ref(false)
const interactiveDeadlineMissed = ref(false)
const showPendingIndicator = computed(() => pending.value && !interactivePreviewVisible.value &&
  (!interactiveSessionPending.value || interactiveDeadlineMissed.value))
let   ro: ResizeObserver | null = null
let   dprMedia: MediaQueryList | null = null
let   renderTimer: ReturnType<typeof setTimeout> | null = null
let   currentRender: AbortController | null = null
let   activeFieldSession: { seq: number; sessionId?: string } | null = null
let   renderSeq = 0
let   slowRenderWarnKey = ''
const renderClientId = `map-${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`
const MIN_FRAME_DIM = 64
const MAX_FRAME_DIM = 4096
const FULL_RENDER_DELAY_MS = 140
// The backend owns this deadline, measured from the lifetime of one native
// field session. It only changes when we begin showing its completed samples;
// it never starts a second, lower-resolution fractal calculation.
const INTERACTIVE_SLOW_AFTER_MS = 450
const INTERACTIVE_SESSION_POLL_MS = 90
const SLOW_RENDER_TARGET_MS = 700
const FAST_RENDER_TARGET_MS = 220
const MIN_ADAPTIVE_QUALITY = 0.35
// This affects only the display density of progressive snapshots. The native
// field calculation always starts at frameW × frameH.
let adaptiveSnapshotQuality = 1

// ── WebGL colorizer state ────────────────────────────────────────────────────
const glCanvas = ref<HTMLCanvasElement | null>(null)
const useGl    = ref(false)
let   glColorizer: GlColorizer | null = null
let   cachedField: { iter: Uint32Array; norm: Float32Array; w: number; h: number; maxIter: number } | null = null

type ViewportSnapshot = {
  scale: number
  aspect: number
  rotationDeg: number
}

let renderedViewport: ViewportSnapshot | null = null

// Offset of the current viewport center from the last rendered frame's
// center, accumulated from the exact interaction deltas. Absolute double
// centers quantize at ~2.2e-16 — useless for interaction below ~2e-13 scale —
// so all preview math runs in this offset space instead (the same idea as
// the backend's perturbation deltas). Refs so previewTransform is reactive
// to pans even when the double center prop cannot change.
const pendingOffsetRe = ref(0)
const pendingOffsetIm = ref(0)

function trackDelta(deltaRe: number, deltaIm: number) {
  pendingOffsetRe.value += deltaRe
  pendingOffsetIm.value += deltaIm
}

watch(() => props.viewEpoch, () => {
  // Non-incremental jump: the offset chain is broken; drop the preview until
  // a fresh frame arrives.
  pendingOffsetRe.value = 0
  pendingOffsetIm.value = 0
  renderedViewport = null
})

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

function clampQuality(q: number): number {
  if (!Number.isFinite(q)) return 1
  return Math.max(MIN_ADAPTIVE_QUALITY, Math.min(1, q))
}

function viewportDepthOctaves(): number {
  const s = props.scale
  return s > 0 && Number.isFinite(s) ? Math.log2(4 / s) : 0
}

function heuristicPreviewQuality(): number {
  const pixels = Math.max(1, frameW.value * frameH.value)
  const iterations = Math.max(1, props.iterations)
  const depth = viewportDepthOctaves()
  let q = 1

  if (pixels > 4_000_000) q = Math.min(q, 0.7)
  else if (pixels > 2_000_000) q = Math.min(q, 0.85)

  if (iterations >= 32768) q = Math.min(q, 0.45)
  else if (iterations >= 16384) q = Math.min(q, 0.55)
  else if (iterations >= 8192) q = Math.min(q, 0.7)
  else if (iterations >= 4096) q = Math.min(q, 0.85)

  if (depth >= 120) q = Math.min(q, 0.45)
  else if (depth >= 80) q = Math.min(q, 0.6)
  else if (depth >= 48) q = Math.min(q, 0.75)

  if (props.transitionTheta !== null || props.transitionFrom || props.transitionVariants?.length) {
    q = Math.min(q, 0.75)
  }
  if (props.metric === 'min_pairwise_dist' || props.metric === 'mandel_ship_agree') {
    q = Math.min(q, 0.75)
  }

  return clampQuality(q)
}

function preferredPreviewQuality(): number {
  return clampQuality(Math.min(adaptiveSnapshotQuality, heuristicPreviewQuality()))
}

function snapshotSizeForQuality(quality: number): { width: number; height: number; quality: number } {
  if (!renderableSize()) return { width: 0, height: 0, quality: 1 }
  const minQuality = Math.max(MIN_FRAME_DIM / frameW.value, MIN_FRAME_DIM / frameH.value)
  const q = Math.min(1, Math.max(minQuality, clampQuality(quality)))
  return {
    width: Math.max(1, Math.round(frameW.value * q)),
    height: Math.max(1, Math.round(frameH.value * q)),
    quality: q,
  }
}

function updateAdaptiveQuality(generatedMs: number, renderQuality = 1) {
  if (!(generatedMs > 0) || !Number.isFinite(generatedMs)) return
  const q = Math.max(MIN_ADAPTIVE_QUALITY, Math.min(1, renderQuality))
  const estimatedFullMs = generatedMs / Math.max(0.01, q * q)
  if (estimatedFullMs > SLOW_RENDER_TARGET_MS) {
    const next = adaptiveSnapshotQuality * Math.sqrt(SLOW_RENDER_TARGET_MS / estimatedFullMs) * 0.95
    adaptiveSnapshotQuality = Math.min(0.95, clampQuality(next))
    return
  }
  if (generatedMs < FAST_RENDER_TARGET_MS && adaptiveSnapshotQuality < 1) {
    adaptiveSnapshotQuality = Math.min(1, adaptiveSnapshotQuality * 1.18)
  }
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
  // A preempt is reserved for an actual view/request invalidation. In
  // particular, crossing the interactive deadline does not abort the native
  // field session: it keeps computing while we show a snapshot of its work.
  const hadRender = currentRender !== null || activeFieldSession !== null
  currentRender?.abort()
  currentRender = null
  activeFieldSession = null
  interactivePreviewVisible.value = false
  interactiveSessionPending.value = false
  interactiveDeadlineMissed.value = false
  // Preserve the pixels already on screen for a transformed interaction
  // preview, but never let a color-only watcher recolorize their old raw
  // field while the replacement view is still inside its debounce window.
  cachedField = null
  renderSeq += 1
  if (hadRender) notifyPreempt(renderSeq)
}

function shortestRotationDelta(currentDeg: number, renderedDeg: number): number {
  const raw = currentDeg - renderedDeg
  if (!Number.isFinite(raw)) return 0
  return ((raw + 180) % 360 + 360) % 360 - 180
}

function previewTransform(): string {
  if (!hasFrame.value || !renderedViewport || domW.value < 16 || domH.value < 16) return 'none'
  const aspect = domW.value / domH.value
  const scaleRatioY = renderedViewport.scale / props.scale
  const scaleRatioX = scaleRatioY * renderedViewport.aspect / aspect
  const rotDelta = shortestRotationDelta(props.rotationDeg ?? 0, renderedViewport.rotationDeg)
  // rendered center − current center, in exact offset space
  const dre = -pendingOffsetRe.value
  const dim = -pendingOffsetIm.value
  // Translation is applied by the matrix before the CSS rotation below, so
  // express it in the rendered frame's axes. The rotation then carries both
  // the old image and this offset into the current viewport axes exactly once.
  const renderedRad = renderedViewport.rotationDeg * Math.PI / 180
  const cosR = Math.cos(renderedRad), sinR = Math.sin(renderedRad)
  const dx = (dre * cosR + dim * sinR) * domW.value / (props.scale * aspect)
  const dy = (-dre * sinR + dim * cosR) * (-domH.value) / props.scale
  const tx = domW.value * (1 - scaleRatioX) * 0.5 + dx
  const ty = domH.value * (1 - scaleRatioY) * 0.5 + dy
  const noChange = Math.abs(scaleRatioX - 1) < 0.000001 &&
    Math.abs(scaleRatioY - 1) < 0.000001 &&
    Math.abs(tx) < 0.01 && Math.abs(ty) < 0.01 && Math.abs(rotDelta) < 0.001
  if (noChange) return 'none'
  const cx = domW.value * 0.5, cy = domH.value * 0.5
  if (Math.abs(rotDelta) > 0.001) {
    return `translate(${cx}px,${cy}px) rotate(${rotDelta}deg) translate(${-cx}px,${-cy}px) matrix(${scaleRatioX},0,0,${scaleRatioY},${tx},${ty})`
  }
  return `matrix(${scaleRatioX}, 0, 0, ${scaleRatioY}, ${tx}, ${ty})`
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

// After the first progressive snapshot is visible, update that same canvas in
// place. This avoids repeatedly toggling the two opacity-transition layers as
// new published tiles arrive from one backend session.
function updateActiveRgbaFrame(data: ArrayBuffer, width: number, height: number): boolean {
  const expectedBytes = width * height * 4
  if (data.byteLength !== expectedBytes) {
    throw new Error(`interactive snapshot size mismatch: got ${data.byteLength}, expected ${expectedBytes}`)
  }
  const target = canvasByIndex(activeCanvas.value)
  resizeCanvas(target, width, height)
  const ctx = target?.getContext('2d')
  if (!ctx) return false
  ctx.putImageData(new ImageData(new Uint8ClampedArray(data), width, height), 0, 0)
  return true
}

function base64ToArrayBuffer(b64: string): Uint8Array {
  const bin = atob(b64)
  const bytes = new Uint8Array(bin.length)
  for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i)
  return bytes
}

/** True when we can use the field+WebGL path instead of render-inline. */
function shouldUseFieldPath(): boolean {
  return useGl.value
    && props.metric === 'escape'
    && (props.colorMode ?? 'direct') === 'direct'
    && props.transitionTheta === null
    && !props.transitionFrom
    && !props.transitionVariants?.length
}

/** Re-colorize cached field data via WebGL — no backend round-trip. */
function recolorize(inPlace = false) {
  if (!cachedField || !glColorizer) return
  const glc = glCanvas.value
  if (!glc) return

  // Resize GL canvas to match field data
  if (glc.width !== cachedField.w || glc.height !== cachedField.h) {
    glc.width  = cachedField.w
    glc.height = cachedField.h
  }

  // Upload and render
  glColorizer.uploadField(cachedField.iter, cachedField.norm, cachedField.w, cachedField.h)
  glColorizer.render({
    colormap: props.colorMap,
    maxIter:  cachedField.maxIter,
    smooth:   props.smooth ?? false,
  })

  // A normal complete render swaps canvases once.  A progressive session has
  // already installed a stable low-density frame, so promote its final raw
  // field into that same canvas and avoid a second opacity transition.
  const next = inPlace ? activeCanvas.value : (activeCanvas.value === 0 ? 1 : 0)
  const target = canvasByIndex(next)
  resizeCanvas(target, cachedField.w, cachedField.h)
  const ctx = target?.getContext('2d')
  if (!ctx) return
  ctx.drawImage(glc, 0, 0)
  if (!inPlace) activeCanvas.value = next
  hasFrame.value = true
}

function fieldSessionState(response: { state?: string; status?: string }): string {
  return response.state ?? response.status ?? ''
}

function waitForSessionPoll(signal: AbortSignal, delayMs = INTERACTIVE_SESSION_POLL_MS): Promise<void> {
  return new Promise((resolve, reject) => {
    if (signal.aborted) {
      reject(new DOMException('The operation was aborted', 'AbortError'))
      return
    }
    const safeDelayMs = Number.isFinite(delayMs)
      ? Math.max(20, Math.min(1000, Math.round(delayMs)))
      : INTERACTIVE_SESSION_POLL_MS
    const timeout = window.setTimeout(() => {
      signal.removeEventListener('abort', onAbort)
      resolve()
    }, safeDelayMs)
    const onAbort = () => {
      window.clearTimeout(timeout)
      reject(new DOMException('The operation was aborted', 'AbortError'))
    }
    signal.addEventListener('abort', onAbort, { once: true })
  })
}

async function renderFrame() {
  if (!renderableSize()) return
  const reqW = frameW.value
  const reqH = frameH.value
  const viewportAspect = domW.value / domH.value
  if (reqW < MIN_FRAME_DIM || reqH < MIN_FRAME_DIM) return
  if (!(viewportAspect > 0) || !Number.isFinite(viewportAspect)) return
  pending.value = true
  error.value = ''

  // Render calls normally follow invalidateCurrentRender(). Keep this local
  // abort as a safety net, but do not preempt here: a preempt belongs only to
  // a real replacement of the current view/request.
  currentRender?.abort()
  const controller = new AbortController()
  currentRender = controller
  const seq = ++renderSeq
  const requestId = `${renderClientId}-${seq}`
  const startOffsetRe = pendingOffsetRe.value
  const startOffsetIm = pendingOffsetIm.value
  const requestRotationDeg = props.rotationDeg ?? 0
  let committedViewport = false
  const commitViewport = (scale: number) => {
    if (!committedViewport) {
      pendingOffsetRe.value -= startOffsetRe
      pendingOffsetIm.value -= startOffsetIm
      committedViewport = true
    }
    renderedViewport = {
      scale,
      aspect: viewportAspect,
      rotationDeg: requestRotationDeg,
    }
  }

  if (shouldUseFieldPath()) {
    // ── Native field session: progressive presentation without duplicate math ──
    const fieldReq: MapFieldRequest = {
      requestId,
      preemptKey: renderClientId,
      preemptSeq: seq,
      centerRe: props.centerRe,
      centerIm: props.centerIm,
      scale: props.scale,
      viewportAspect,
      width: reqW,
      height: reqH,
      iterations: props.iterations,
      variant: props.variant,
      metric: props.metric,
      pairwiseCap: props.pairwiseCap,
      julia: props.julia,
      juliaRe: props.juliaRe ?? 0,
      juliaIm: props.juliaIm ?? 0,
    }
    if (props.engine) fieldReq.engine = props.engine
    if (props.scalarType) fieldReq.scalarType = props.scalarType
    if (props.rotationDeg) fieldReq.rotationDeg = props.rotationDeg
    if (props.centerReStr) fieldReq.centerReStr = props.centerReStr
    if (props.centerImStr) fieldReq.centerImStr = props.centerImStr

    const sessionReq: MapFieldSessionStartRequest = {
      ...fieldReq,
      colorMap: props.colorMap,
      smooth: props.smooth,
      colorMode: 'direct',
      slowAfterMs: INTERACTIVE_SLOW_AFTER_MS,
    }

    // Mark the session before the start response arrives so a view change can
    // preempt the backend work even while this request is in flight.
    activeFieldSession = { seq }
    interactiveSessionPending.value = true
    interactiveDeadlineMissed.value = false
    try {
      let status: MapFieldSessionStatus
      for (;;) {
        if (seq !== renderSeq || activeFieldSession?.seq !== seq) return
        try {
          status = await api.mapFieldSessionStart(sessionReq, controller.signal)
          break
        } catch (e: any) {
          // A replacement view never creates a third detached native render.
          // The backend has already asked any superseded worker to stop, and
          // returns 429 until its slot/buffer is genuinely released. Keep the
          // current transformed frame visible and retry this identical native
          // request rather than falling back to a fresh low-resolution job.
          if (e?.status !== 429 || e?.data?.retryable !== true) throw e
          const retryAfterMs = Number(e?.data?.retryAfterMs)
          await waitForSessionPoll(controller.signal, retryAfterMs)
        }
      }
      if (seq !== renderSeq || activeFieldSession?.seq !== seq) return
      if (status.requestId && status.requestId !== requestId) return
      if (status.viewportAspect !== undefined &&
          Math.abs(status.viewportAspect - viewportAspect) >
            1e-12 * Math.max(1, Math.abs(viewportAspect))) {
        throw new Error('interactive field viewport aspect mismatch')
      }
      let state = fieldSessionState(status)
      if (state === 'cancelled') return
      if (state === 'failed') throw new Error(status.error ?? 'interactive field session failed')
      const sessionId = status.sessionId
      if (!sessionId) throw new Error('interactive field session missing sessionId')
      activeFieldSession = { seq, sessionId }

      let snapshotShown = false
      let snapshotRevision = -1
      let snapshotAttemptedWithoutRevision = false
      let lastSnapshotAt = 0
      while (seq === renderSeq && activeFieldSession?.seq === seq) {
        state = fieldSessionState(status)
        if (state === 'cancelled') return
        if (state === 'failed') throw new Error(status.error ?? 'interactive field session failed')

        if (state === 'completed') {
          let result: MapFieldSessionResult
          try {
            result = await api.mapFieldSessionResult(sessionId, controller.signal)
          } catch (e: any) {
            // Result encoding is intentionally serialized server-side to
            // bound temporary transport memory across multiple explorer tabs.
            // This is transport back-pressure only; the completed native
            // field remains intact and the same session is retried.
            if (e?.status === 429 && e?.data?.retryable === true) {
              await waitForSessionPoll(controller.signal, Number(e?.data?.retryAfterMs))
              status = await api.mapFieldSessionStatus(sessionId, controller.signal)
              continue
            }
            throw e
          }
          if (seq !== renderSeq || activeFieldSession?.seq !== seq) return
          const resultState = fieldSessionState(result)
          if (resultState === 'cancelled') return
          if (resultState === 'failed') throw new Error(result.error ?? 'interactive field session failed')
          if (resultState !== 'completed') {
            await waitForSessionPoll(controller.signal)
            status = await api.mapFieldSessionStatus(sessionId, controller.signal)
            continue
          }
          if (!result.iterB64 || !result.finalMagB64 || !result.width || !result.height) {
            throw new Error('interactive field result missing iterB64/finalMagB64')
          }

          const iterBuf = new Uint32Array(base64ToArrayBuffer(result.iterB64).buffer)
          const normBuf = new Float32Array(base64ToArrayBuffer(result.finalMagB64).buffer)
          const pixelCount = result.width * result.height
          if (!Number.isSafeInteger(pixelCount) || iterBuf.length !== pixelCount || normBuf.length !== pixelCount) {
            throw new Error('interactive field result buffer size mismatch')
          }
          cachedField = {
            iter: iterBuf,
            norm: normBuf,
            w: result.width,
            h: result.height,
            maxIter: result.maxIter ?? props.iterations,
          }
          // The backend keeps the immutable native field until this browser
          // has fully decoded it. A failed/aborted result response sends no
          // acknowledgement, so the same session can still be retrieved.
          void api.mapFieldSessionAcknowledge(sessionId, requestId).catch(() => {})
          if (seq !== renderSeq || activeFieldSession?.seq !== seq) return
          commitViewport(fieldReq.scale)
          recolorize(snapshotShown)
          const generatedMs = result.generatedMs ?? status.generatedMs ?? 0
          updateAdaptiveQuality(generatedMs)
          maybeWarnSlowRender(generatedMs)
          emit('rendered', {
            generatedMs,
            artifactId: '',
            engineUsed: result.engineUsed,
            scalarUsed: result.scalarUsed,
          })
          return
        }

        // The backend, not a frontend timer, determines when the session has
        // crossed the interactive budget. A snapshot is a display-only view
        // of published samples from this exact native-size calculation.
        if (status.deadlinePassed) {
          interactiveDeadlineMissed.value = true
          const revision = status.revision
          const newPublishedData = revision === undefined
            ? !snapshotAttemptedWithoutRevision
            : revision !== snapshotRevision
          const snapshotThrottlePassed = Date.now() - lastSnapshotAt >= 160
          if (newPublishedData && snapshotThrottlePassed) {
            if (revision === undefined) snapshotAttemptedWithoutRevision = true
            else snapshotRevision = revision
            lastSnapshotAt = Date.now()
            // Once the backend has observed an actual latency miss, lower the
            // *presentation* resolution even for a shallow/default view. The
            // native field keeps its full dimensions and continues unchanged.
            const snapshotSize = snapshotSizeForQuality(Math.min(0.7, preferredPreviewQuality()))
            const snapshot = await api.mapFieldSessionSnapshot(
              sessionId,
              snapshotSize.width,
              snapshotSize.height,
              { colorMap: props.colorMap, smooth: props.smooth },
              controller.signal,
            )
            if (seq !== renderSeq || activeFieldSession?.seq !== seq) return
            const snapshotState = fieldSessionState(snapshot)
            if (snapshotState === 'cancelled') return
            if (snapshotState === 'failed') throw new Error(snapshot.error ?? 'interactive field session failed')
            status = snapshot
            if (snapshotState !== 'completed' && snapshot.previewAvailable && snapshot.rgbaB64 &&
                snapshot.previewWidth && snapshot.previewHeight) {
              const preview = base64ToArrayBuffer(snapshot.rgbaB64)
              let painted = false
              if (snapshotShown) {
                painted = updateActiveRgbaFrame(preview.buffer as ArrayBuffer, snapshot.previewWidth, snapshot.previewHeight)
              } else {
                const next = drawRgbaFrame(preview.buffer as ArrayBuffer, snapshot.previewWidth, snapshot.previewHeight)
                if (next !== null) {
                  activeCanvas.value = next
                  painted = true
                }
              }
              if (painted && seq === renderSeq) {
                commitViewport(fieldReq.scale)
                hasFrame.value = true
                snapshotShown = true
                interactivePreviewVisible.value = true
              }
            }
            continue
          }
        }

        await waitForSessionPoll(controller.signal)
        status = await api.mapFieldSessionStatus(sessionId, controller.signal)
      }
    } catch (e: any) {
      if (seq === renderSeq && e?.name !== 'AbortError') error.value = e?.message ?? String(e)
    } finally {
      if (activeFieldSession?.seq === seq) activeFieldSession = null
      if (currentRender === controller) currentRender = null
      if (seq === renderSeq) {
        pending.value = false
        interactivePreviewVisible.value = false
        interactiveSessionPending.value = false
        interactiveDeadlineMissed.value = false
      }
    }
    return
  }

  // ── Inline path (non-escape metrics, equalized color, transitions): one full render ──
  const req: MapRenderRequest = {
    requestId,
    preemptKey: renderClientId,
    preemptSeq: seq,
    centerRe: props.centerRe,
    centerIm: props.centerIm,
    scale: props.scale,
    viewportAspect,
    width: reqW,
    height: reqH,
    iterations: props.iterations,
    variant: props.variant,
    metric: props.metric,
    colorMap: props.colorMap,
    smooth: props.smooth,
    colorMode: props.colorMode,
    cyclesPerOctave: props.cyclesPerOctave,
    pairwiseCap: props.pairwiseCap,
    julia: props.julia,
    juliaRe: props.juliaRe ?? 0,
    juliaIm: props.juliaIm ?? 0,
  }
  if (props.transitionThetaMilliDeg !== null && props.transitionThetaMilliDeg !== undefined) {
    req.transitionThetaMilliDeg = props.transitionThetaMilliDeg
    req.transitionTheta = props.transitionThetaMilliDeg * Math.PI / 180000
  } else if (props.transitionTheta !== null) {
    req.transitionTheta = props.transitionTheta
  }
  if (props.transitionFrom) req.transitionFrom = props.transitionFrom
  if (props.transitionTo) req.transitionTo = props.transitionTo
  if (props.transitionVariants?.length) {
    req.transitionVariants = props.transitionVariants
    req.transitionWeights = props.transitionWeights ?? props.transitionVariants.map(() => 1)
  }
  if (props.engine) req.engine = props.engine
  if (props.scalarType) req.scalarType = props.scalarType
  if (props.centerReStr) (req as any).centerReStr = props.centerReStr
  if (props.centerImStr) (req as any).centerImStr = props.centerImStr
  if (props.rotationDeg) req.rotationDeg = props.rotationDeg

  try {
    cachedField = null
    const resp = await api.mapRenderInline(req, controller.signal) as any
    if (seq !== renderSeq || (resp.requestId && resp.requestId !== requestId)) return
    if (resp.status === 'cancelled' || !resp.data) return
    if (resp.pixelFormat && resp.pixelFormat !== 'rgba8') {
      throw new Error(`unsupported inline frame format: ${resp.pixelFormat}`)
    }
    const responseWidth = resp.width || reqW
    const responseHeight = resp.height || reqH
    const next = drawRgbaFrame(resp.data, responseWidth, responseHeight)
    if (next === null || seq !== renderSeq) return
    commitViewport(req.scale)
    hasFrame.value = true
    activeCanvas.value = next
    updateAdaptiveQuality(resp.generatedMs)
    maybeWarnSlowRender(resp.generatedMs)
    emit('rendered', {
      generatedMs: resp.generatedMs,
      artifactId: '',
      engineUsed: resp.engineUsed,
      scalarUsed: resp.scalarUsed,
    })
  } catch (e: any) {
    if (seq === renderSeq && e?.name !== 'AbortError') error.value = e?.message ?? String(e)
  } finally {
    if (currentRender === controller) currentRender = null
    if (seq === renderSeq) pending.value = false
  }
}

function scheduleRender(delay = FULL_RENDER_DELAY_MS) {
  invalidateCurrentRender()
  if (renderableSize()) {
    pending.value = true
    error.value = ''
  }
  if (renderTimer) clearTimeout(renderTimer)
  renderTimer = setTimeout(() => {
    renderTimer = null
    void renderFrame()
  }, delay)
}

// ── Watchers ──────────────────────────────────────────────────────────────────

// Compute watch — viewport, fractal parameters, frame size → full backend re-fetch
watch(() => [
  props.centerRe, props.centerIm, props.scale,
  props.centerReStr, props.centerImStr, props.viewEpoch,
  props.variant, props.metric,
  props.colorMode, props.colorMode === 'direct' ? null : props.cyclesPerOctave,
  props.iterations, props.pairwiseCap, props.julia, props.juliaRe, props.juliaIm,
  props.engine, props.scalarType, props.rotationDeg, props.transitionTheta, props.transitionThetaMilliDeg,
  props.transitionFrom, props.transitionTo,
  props.transitionVariants?.join(','), props.transitionWeights?.join(','),
  frameW.value, frameH.value,
], () => scheduleRender())

// Color-only watch — instant re-colorize from cached field data when possible
watch(() => [props.colorMap, props.smooth], () => {
  if (useGl.value && shouldUseFieldPath()) {
    // Palette/smoothing do not alter the native escape field. Keep a live
    // session running. `cachedField` may still belong to the previous view,
    // so never draw it over the current session's progressive snapshot.
    // Subsequent snapshots carry the newest presentation options and the
    // completed raw field is recolorized below.
    if (activeFieldSession) return
    if (cachedField) {
      recolorize()
      return
    }
  }
  scheduleRender()
})

// ── Lifecycle ─────────────────────────────────────────────────────────────────

onMounted(() => {
  if (!wrapper.value || !canvasA.value || !canvasB.value) return

  // Initialize WebGL colorizer on the hidden canvas
  if (glCanvas.value && isWebGL2Available()) {
    try {
      glColorizer = new GlColorizer(glCanvas.value)
      useGl.value = true
    } catch {
      glColorizer = null
      useGl.value = false
    }
  }

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
  // Release WebGL resources
  if (glColorizer) {
    glColorizer.dispose()
    glColorizer = null
    useGl.value = false
  }
  cachedField = null
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
  const factor  = e.deltaY > 0 ? 1.25 : 0.8
  const newScale = props.scale * factor
  const dScale = props.scale - newScale
  const dx = (px - 0.5) * dScale * aspect
  const dy = -(py - 0.5) * dScale
  const rad = (props.rotationDeg ?? 0) * Math.PI / 180
  const cosR = Math.cos(rad), sinR = Math.sin(rad)
  const deltaRe = dx * cosR - dy * sinR
  const deltaIm = dx * sinR + dy * cosR
  trackDelta(deltaRe, deltaIm)
  emit('viewport-change', {
    centerRe: props.centerRe + deltaRe,
    centerIm: props.centerIm + deltaIm,
    scale:    newScale,
    deltaRe,
    deltaIm,
  })
}

type PointerPoint = { x: number; y: number }
type PinchStart = {
  distance: number
  scale: number
  prevOffsetRe: number
  prevOffsetIm: number
}

const activePointers = new Map<number, PointerPoint>()
let dragging  = false
let dragMoved = false
let dragStart = { x: 0, y: 0, cx: 0, cy: 0, sc: 0 }
let prevDragPos = { x: 0, y: 0 }
let pinchStart: PinchStart | null = null

function screenToWorld(clientX: number, clientY: number): { re: number; im: number } | null {
  if (!wrapper.value) return null
  const rect   = wrapper.value.getBoundingClientRect()
  const aspect = rect.width / rect.height
  const px     = (clientX - rect.left) / rect.width
  const py     = (clientY - rect.top)  / rect.height
  const dx = (px - 0.5) * props.scale * aspect
  const dy = -(py - 0.5) * props.scale
  const rad = (props.rotationDeg ?? 0) * Math.PI / 180
  const cosR = Math.cos(rad), sinR = Math.sin(rad)
  return {
    re: props.centerRe + dx * cosR - dy * sinR,
    im: props.centerIm + dx * sinR + dy * cosR,
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
  if (!wrapper.value) {
    pinchStart = null
    return
  }
  const rect = wrapper.value.getBoundingClientRect()
  const midpoint = pointerMidpoint(points)
  const distance = pointerDistance(points)
  if (distance <= 0) {
    pinchStart = null
    return
  }
  // Track the pinch anchor as a rotated OFFSET from the center (offset-space
  // stays exact in doubles at any zoom depth, unlike absolute coordinates),
  // so the emitted deltas preserve the BigDec center precision upstream.
  const px = (midpoint.x - rect.left) / rect.width
  const py = (midpoint.y - rect.top) / rect.height
  const aspect = rect.width / rect.height
  const dx = (px - 0.5) * props.scale * aspect
  const dy = -(py - 0.5) * props.scale
  const rad = (props.rotationDeg ?? 0) * Math.PI / 180
  const cosR = Math.cos(rad), sinR = Math.sin(rad)
  pinchStart = {
    distance,
    scale: props.scale,
    prevOffsetRe: dx * cosR - dy * sinR,
    prevOffsetIm: dx * sinR + dy * cosR,
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
  const dx = (px - 0.5) * newScale * aspect
  const dy = -(py - 0.5) * newScale
  const rad = (props.rotationDeg ?? 0) * Math.PI / 180
  const cosR = Math.cos(rad), sinR = Math.sin(rad)
  const offsetRe = dx * cosR - dy * sinR
  const offsetIm = dx * sinR + dy * cosR
  // Keeping the anchor's world point fixed on screen:
  //   center_new = center_old + (offset_prev - offset_new)
  const deltaRe = pinchStart.prevOffsetRe - offsetRe
  const deltaIm = pinchStart.prevOffsetIm - offsetIm
  pinchStart.prevOffsetRe = offsetRe
  pinchStart.prevOffsetIm = offsetIm
  trackDelta(deltaRe, deltaIm)
  emit('viewport-change', {
    centerRe: props.centerRe + deltaRe,
    centerIm: props.centerIm + deltaIm,
    scale: newScale,
    deltaRe,
    deltaIm,
  })
}

function offsetToScreen(dre: number, dim: number): { x: number; y: number; visible: boolean } {
  const w = Math.max(1, domW.value)
  const h = Math.max(1, domH.value)
  const aspect = w / h
  const rad = (props.rotationDeg ?? 0) * Math.PI / 180
  const cosR = Math.cos(rad), sinR = Math.sin(rad)
  const dx = dre * cosR + dim * sinR
  const dy = -dre * sinR + dim * cosR
  const x = (0.5 + dx / (props.scale * aspect)) * w
  const y = (0.5 - dy / props.scale) * h
  return { x, y, visible: x >= -8 && x <= w + 8 && y >= -8 && y <= h + 8 }
}

function worldToScreen(re: number, im: number): { x: number; y: number; visible: boolean } {
  return offsetToScreen(re - props.centerRe, im - props.centerIm)
}

function markerStyle(p: SpecialPointEnumResult) {
  // Deep-zoom points carry a BigDec-computed offset; the double coordinates
  // collapse onto the center below ~1e-13 scales.
  const pos = p.offsetRe !== undefined && p.offsetIm !== undefined
    ? offsetToScreen(p.offsetRe, p.offsetIm)
    : worldToScreen(p.re, p.im)
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
  prevDragPos = { x: e.clientX, y: e.clientY }
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
  const rad = (props.rotationDeg ?? 0) * Math.PI / 180
  const cosR = Math.cos(rad), sinR = Math.sin(rad)
  const incDx = e.clientX - prevDragPos.x
  const incDy = e.clientY - prevDragPos.y
  prevDragPos = { x: e.clientX, y: e.clientY }
  const rawIncRe = -(incDx / rect.width)  * dragStart.sc * aspect
  const rawIncIm =  (incDy / rect.height) * dragStart.sc
  const deltaRe = rawIncRe * cosR - rawIncIm * sinR
  const deltaIm = rawIncRe * sinR + rawIncIm * cosR
  const rawTotRe = -(dx / rect.width)  * dragStart.sc * aspect
  const rawTotIm =  (dy / rect.height) * dragStart.sc
  const totalRe = rawTotRe * cosR - rawTotIm * sinR
  const totalIm = rawTotRe * sinR + rawTotIm * cosR
  trackDelta(deltaRe, deltaIm)
  emit('viewport-change', {
    centerRe: dragStart.cx + totalRe,
    centerIm: dragStart.cy + totalIm,
    scale:    dragStart.sc,
    deltaRe,
    deltaIm,
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
    prevDragPos = { x: next.x, y: next.y }
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

const exportFrameStyle = computed(() => {
  if (!props.showExportFrame || !props.exportFrameWidth || !props.exportFrameHeight) return null
  const vw = domW.value, vh = domH.value
  if (vw < 16 || vh < 16) return null
  const exportAspect = props.exportFrameWidth / props.exportFrameHeight
  const viewportAspect = vw / vh
  let frameW: number, frameH: number
  if (exportAspect > viewportAspect) {
    frameW = vw
    frameH = vw / exportAspect
  } else {
    frameH = vh
    frameW = vh * exportAspect
  }
  const left = (vw - frameW) / 2
  const top = (vh - frameH) / 2
  return {
    left: `${left}px`,
    top: `${top}px`,
    width: `${frameW}px`,
    height: `${frameH}px`,
  }
})
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
    <canvas ref="glCanvas" style="display:none"></canvas>
    <canvas
      ref="canvasA"
      class="frame"
      :class="{ active: activeCanvas === 0, stale: showPendingIndicator && activeCanvas === 0 }"
      :style="canvasStyle(0)"
    />
    <canvas
      ref="canvasB"
      class="frame"
      :class="{ active: activeCanvas === 1, stale: showPendingIndicator && activeCanvas === 1 }"
      :style="canvasStyle(1)"
    />
    <div v-if="showPendingIndicator" class="busy-line"></div>
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
    <div v-if="showExportFrame && exportFrameStyle"
         class="export-frame"
         :style="exportFrameStyle" />
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

.export-frame {
  position: absolute;
  pointer-events: none;
  border: 2px dashed rgba(255, 255, 255, 0.7);
  box-shadow: 0 0 0 9999px rgba(0, 0, 0, 0.35);
  z-index: 10;
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
