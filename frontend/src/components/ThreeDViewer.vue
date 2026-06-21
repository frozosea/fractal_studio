<script setup lang="ts">
// ThreeDViewer.vue — three.js viewer with three render modes:
//
//   HS field mode  : frontend-built PlaneGeometry from float64 height field.
//                    Camera top-down, pan+zoom only (no orbit rotation).
//                    zScale applied client-side without re-fetching field data.
//   GLB mode       : loads a glTF/GLB mesh artifact URL.
//   Voxel mode     : Minecraft-style surface mesh for M↔B transition.
//                    Color encodes depth: byte=1 (deep) → dark amber,
//                                        byte=255 (near surface) → bright amber.

import { onMounted, onBeforeUnmount, ref, watch } from 'vue'
import * as THREE from 'three'
import { GLTFLoader } from 'three/examples/jsm/loaders/GLTFLoader.js'
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js'
import type { TransitionVoxelResponse, HsFieldResponse } from '../api'
import { isLight, clearColor } from '../theme'
import { lang } from '../i18n'

const props = defineProps<{
  glbUrl?: string | null
  voxelData?: TransitionVoxelResponse | null
  hsFieldData?: HsFieldResponse | null
  zScale?: number
  viewMode?: 'hs' | 'transition'
  loading?: boolean
  extent?: number
}>()

const emit = defineEmits<{
  (e: 'pan',  ndx: number, ndy: number): void
  (e: 'zoom', factor: number): void
}>()

const canvasEl = ref<HTMLCanvasElement | null>(null)

let renderer: THREE.WebGLRenderer | null = null
let scene:    THREE.Scene | null = null
let camera:   THREE.PerspectiveCamera | null = null
let controls: OrbitControls | null = null
let animId:   number | null = null
let meshGroup: THREE.Group | null = null
let voxelMesh: THREE.Mesh | null = null
let hsMesh:    THREE.Mesh | null = null
let keyLight:  THREE.DirectionalLight | null = null
let fillLight: THREE.DirectionalLight | null = null

// Cache raw float64 field values for z-scale rebuildling without re-fetch
let cachedField:  Float64Array | null = null
let cachedW = 0, cachedH = 0

let ro: ResizeObserver | null = null

// ── HS pan/zoom interaction state ─────────────────────────────────────────────
let hsDragging = false
let hsDragStartX = 0
let hsDragStartY = 0

function onHsPointerDown(e: PointerEvent) {
  hsDragging   = true
  hsDragStartX = e.clientX
  hsDragStartY = e.clientY
  ;(e.target as HTMLElement).setPointerCapture(e.pointerId)
}

function onHsPointerMove(e: PointerEvent) {
  if (!hsDragging || !renderer) return
  const dx = e.clientX - hsDragStartX
  const dy = e.clientY - hsDragStartY
  hsDragStartX = e.clientX
  hsDragStartY = e.clientY
  const el = renderer.domElement
  emit('pan', dx / el.clientWidth, dy / el.clientHeight)
}

function onHsPointerUp(e: PointerEvent) {
  hsDragging = false
  ;(e.target as HTMLElement).releasePointerCapture(e.pointerId)
}

function onHsWheel(e: WheelEvent) {
  e.preventDefault()
  const factor = Math.pow(0.9, e.deltaY / 100)
  emit('zoom', factor)
}

// ── Init ─────────────────────────────────────────────────────────────────────

function initThree() {
  const el = canvasEl.value!
  renderer = new THREE.WebGLRenderer({ canvas: el, antialias: true })
  renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2))
  renderer.setClearColor(clearColor(), 1)

  scene = new THREE.Scene()

  camera = new THREE.PerspectiveCamera(45, el.clientWidth / el.clientHeight, 0.001, 100)
  camera.position.set(0, 0.8, 3.2)

  keyLight = new THREE.DirectionalLight(0xd7dae0, 1.8)
  keyLight.position.set(1.5, 2, 2)
  scene.add(keyLight)
  fillLight = new THREE.DirectionalLight(0xd7dae0, 0.5)
  fillLight.position.set(-2, 1, -1)
  scene.add(fillLight)
  scene.add(new THREE.AmbientLight(0xd7dae0, 0.25))

  controls = new OrbitControls(camera, renderer.domElement)
  controls.enableDamping  = true
  controls.dampingFactor  = 0.08
  controls.minDistance    = 0.05
  controls.maxDistance    = 20

  applyViewMode(props.viewMode)

  ro = new ResizeObserver(() => resizeRenderer())
  ro.observe(el.parentElement!)
  resizeRenderer()
  loop()
}

function applyViewMode(mode?: 'hs' | 'transition') {
  if (!controls || !camera || !renderer) return
  const el = renderer.domElement
  if (mode === 'hs') {
    // Disable OrbitControls; use custom pointer events for complex-plane pan/zoom
    controls.enabled = false
    el.addEventListener('pointerdown', onHsPointerDown)
    el.addEventListener('pointermove', onHsPointerMove)
    el.addEventListener('pointerup',   onHsPointerUp)
    el.addEventListener('wheel',       onHsWheel, { passive: false })
    camera.position.set(0, 3.0, 0.0)
    controls.target.set(0, 0, 0)
    controls.update()
  } else {
    // Re-enable OrbitControls; remove custom HS listeners
    controls.enabled = true
    el.removeEventListener('pointerdown', onHsPointerDown)
    el.removeEventListener('pointermove', onHsPointerMove)
    el.removeEventListener('pointerup',   onHsPointerUp)
    el.removeEventListener('wheel',       onHsWheel)
    camera.position.set(0, 0.8, 3.2)
    controls.target.set(0, 0, 0)
    controls.update()
  }
}

function resizeRenderer() {
  if (!renderer || !camera || !canvasEl.value) return
  const p = canvasEl.value.parentElement!
  renderer.setSize(p.clientWidth, p.clientHeight, false)
  camera.aspect = p.clientWidth / p.clientHeight
  camera.updateProjectionMatrix()
}

function loop() {
  animId = requestAnimationFrame(loop)
  controls!.update()
  renderer!.render(scene!, camera!)
}

function resetCamera() {
  controls!.reset()
  applyViewMode(props.viewMode)
}

// Position camera and lights for the voxel (M↔B) view based on extent.
function positionForExtent(extent: number) {
  if (!camera || !controls || !keyLight || !fillLight) return
  const d = extent * 2.5
  camera.position.set(d * 0.6, d * 0.5, d)
  camera.near = extent * 0.001
  camera.far  = extent * 20
  camera.updateProjectionMatrix()
  controls.minDistance = extent * 0.05
  controls.maxDistance = extent * 15
  controls.target.set(0, 0, 0)
  controls.update()
  keyLight.position.set(extent * 0.75, extent, extent)
  fillLight.position.set(-extent, extent * 0.5, -extent * 0.5)
}

// ── GLB ──────────────────────────────────────────────────────────────────────

function clearMesh() {
  if (meshGroup && scene) {
    scene.remove(meshGroup)
    meshGroup.traverse(o => {
      if (o instanceof THREE.Mesh) {
        o.geometry.dispose()
        if (Array.isArray(o.material)) o.material.forEach(m => m.dispose())
        else o.material.dispose()
      }
    })
    meshGroup = null
  }
}

const loader = new GLTFLoader()

async function loadGlb(url: string) {
  clearMesh(); clearVoxels(); clearHsMesh()
  try {
    const gltf = await loader.loadAsync(url)
    meshGroup = new THREE.Group()
    gltf.scene.traverse(child => {
      if (child instanceof THREE.Mesh) {
        child.material = new THREE.MeshStandardMaterial({
          color: 0xc8cdd5, roughness: 0.7, metalness: 0.1, side: THREE.DoubleSide,
        })
        meshGroup!.add(child.clone())
      }
    })
    const box    = new THREE.Box3().setFromObject(meshGroup)
    const center = box.getCenter(new THREE.Vector3())
    const size   = box.getSize(new THREE.Vector3()).length()
    meshGroup.position.sub(center)
    if (size > 0) meshGroup.scale.setScalar(2.0 / size)
    scene!.add(meshGroup)
    resetCamera()
  } catch (e) {
    console.error('ThreeDViewer: GLB load failed', e)
  }
}

// ── Voxels ────────────────────────────────────────────────────────────────────

function clearVoxels() {
  if (voxelMesh && scene) {
    scene.remove(voxelMesh)
    voxelMesh.geometry.dispose()
    ;(voxelMesh.material as THREE.Material).dispose()
    voxelMesh = null
  }
}

const DARK   = new THREE.Color(0x1a0800)
const BRIGHT = new THREE.Color(0xf0a030)
const TMP    = new THREE.Color()

function decodeB64Bytes(b64: string): Uint8Array {
  const bin = atob(b64)
  const out = new Uint8Array(bin.length)
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i)
  return out
}

function buildVoxels(data: TransitionVoxelResponse) {
  clearMesh(); clearVoxels(); clearHsMesh()

  const posBytes   = decodeB64Bytes(data.posB64)
  const normBytes  = decodeB64Bytes(data.normB64)
  const depthBytes = decodeB64Bytes(data.depthB64)

  const faceCount  = depthBytes.length
  if (faceCount === 0) return

  const posF32 = new Float32Array(posBytes.buffer, posBytes.byteOffset, posBytes.byteLength / 4)

  const norF32 = new Float32Array(faceCount * 4 * 3)
  const normI8 = new Int8Array(normBytes.buffer, normBytes.byteOffset, normBytes.byteLength)
  for (let f = 0; f < faceCount; f++) {
    const nx = normI8[f * 3 + 0]
    const ny = normI8[f * 3 + 1]
    const nz = normI8[f * 3 + 2]
    for (let v = 0; v < 4; v++) {
      norF32[(f * 4 + v) * 3 + 0] = nx
      norF32[(f * 4 + v) * 3 + 1] = ny
      norF32[(f * 4 + v) * 3 + 2] = nz
    }
  }

  const colF32 = new Float32Array(faceCount * 4 * 3)
  for (let f = 0; f < faceCount; f++) {
    const t = Math.pow((depthBytes[f] - 1) / 254, 0.55)
    TMP.copy(DARK).lerp(BRIGHT, t)
    for (let v = 0; v < 4; v++) {
      colF32[(f * 4 + v) * 3 + 0] = TMP.r
      colF32[(f * 4 + v) * 3 + 1] = TMP.g
      colF32[(f * 4 + v) * 3 + 2] = TMP.b
    }
  }

  const idxArr = new Uint32Array(faceCount * 6)
  for (let f = 0; f < faceCount; f++) {
    const b = f * 4
    idxArr[f * 6 + 0] = b;     idxArr[f * 6 + 1] = b + 1; idxArr[f * 6 + 2] = b + 2
    idxArr[f * 6 + 3] = b;     idxArr[f * 6 + 4] = b + 2; idxArr[f * 6 + 5] = b + 3
  }

  const geo = new THREE.BufferGeometry()
  geo.setAttribute('position', new THREE.BufferAttribute(posF32,  3))
  geo.setAttribute('normal',   new THREE.BufferAttribute(norF32,  3))
  geo.setAttribute('color',    new THREE.BufferAttribute(colF32,  3))
  geo.setIndex(new THREE.BufferAttribute(idxArr, 1))

  voxelMesh = new THREE.Mesh(
    geo,
    new THREE.MeshStandardMaterial({ vertexColors: true, roughness: 0.85, metalness: 0.05 })
  )
  scene!.add(voxelMesh)
  const ext = props.extent ?? data.resolution * 0.03
  positionForExtent(ext)
}

// ── HS Height Field ───────────────────────────────────────────────────────────
// Builds a PlaneGeometry-style grid from the float64 height field.
// zScale is applied client-side; rebuilding geometry from cached field is fast.

function clearHsMesh() {
  if (hsMesh && scene) {
    scene.remove(hsMesh)
    hsMesh.geometry.dispose()
    ;(hsMesh.material as THREE.Material).dispose()
    hsMesh = null
  }
  cachedField = null
  cachedW = 0
  cachedH = 0
}

function buildHsField(data: HsFieldResponse, zScale: number) {
  clearMesh(); clearVoxels(); clearHsMesh()

  const W = data.width
  const H = data.height

  // Decode base64 float64 array
  const bytes = decodeB64Bytes(data.fieldB64)
  const f64 = new Float64Array(bytes.buffer, bytes.byteOffset, bytes.byteLength / 8)

  // Cache for z-scale rebuild
  cachedField = f64
  cachedW = W
  cachedH = H

  hsMesh = buildHsGeometry(f64, W, H, data.fieldMin, data.fieldMax, zScale)
  scene!.add(hsMesh)
  resetCamera()
}

// Build the actual THREE.Mesh from the float64 field
function buildHsGeometry(
  f64: Float64Array, W: number, H: number,
  fmin: number, fmax: number, zScale: number
): THREE.Mesh {
  const denom = fmax > fmin ? fmax - fmin : 1.0
  const vertCount = W * H

  const positions = new Float32Array(vertCount * 3)
  const uvs       = new Float32Array(vertCount * 2)

  // Build vertex positions: XZ on the grid, Y = field value * zScale
  for (let row = 0; row < H; row++) {
    for (let col = 0; col < W; col++) {
      const idx = row * W + col
      const x = (col / (W - 1)) - 0.5
      const z = (row / (H - 1)) - 0.5
      const f01 = (f64[idx] - fmin) / denom
      const y = f01 * zScale

      positions[idx * 3 + 0] = x
      positions[idx * 3 + 1] = y
      positions[idx * 3 + 2] = z

      uvs[idx * 2 + 0] = col / (W - 1)
      uvs[idx * 2 + 1] = 1.0 - row / (H - 1)
    }
  }

  // Build indices (two triangles per grid cell)
  const idxArr = new Uint32Array((W - 1) * (H - 1) * 6)
  let ii = 0
  for (let row = 0; row < H - 1; row++) {
    for (let col = 0; col < W - 1; col++) {
      const a = row * W + col
      const b = row * W + col + 1
      const c = (row + 1) * W + col
      const d = (row + 1) * W + col + 1
      idxArr[ii++] = a; idxArr[ii++] = c; idxArr[ii++] = b
      idxArr[ii++] = b; idxArr[ii++] = c; idxArr[ii++] = d
    }
  }

  const geo = new THREE.BufferGeometry()
  geo.setAttribute('position', new THREE.BufferAttribute(positions, 3))
  geo.setAttribute('uv',       new THREE.BufferAttribute(uvs,       2))
  geo.setIndex(new THREE.BufferAttribute(idxArr, 1))
  geo.computeVertexNormals()

  const mat = new THREE.MeshStandardMaterial({
    color:     0xf0a030,
    roughness: 0.6,
    metalness: 0.1,
    side:      THREE.DoubleSide,
  })

  const mesh = new THREE.Mesh(geo, mat)
  mesh.userData.fieldMin = fmin
  mesh.userData.fieldMax = fmax
  return mesh
}

// Rebuild Y coordinates in place when zScale changes, without a new fetch.
function applyZScaleInPlace(zScale: number) {
  if (!hsMesh || !cachedField || cachedW === 0) return

  const geo = hsMesh.geometry
  const pos = geo.getAttribute('position') as THREE.BufferAttribute
  const W = cachedW
  const H = cachedH
  const fmin = hsMesh.userData.fieldMin
  const fmax = hsMesh.userData.fieldMax
  const denom = fmax > fmin ? fmax - fmin : 1.0

  for (let row = 0; row < H; row++) {
    for (let col = 0; col < W; col++) {
      const idx = row * W + col
      const f01 = (cachedField[idx] - fmin) / denom
      pos.setY(idx, f01 * zScale)
    }
  }
  pos.needsUpdate = true
  geo.computeVertexNormals()
}

// ── Watchers ──────────────────────────────────────────────────────────────────

watch(() => props.glbUrl, url => {
  if (url) loadGlb(url)
  else { clearMesh(); clearVoxels() }
})

watch(() => props.voxelData, data => {
  if (data) buildVoxels(data)
  else clearVoxels()
})

watch(() => props.hsFieldData, data => {
  if (data) buildHsField(data, props.zScale ?? 0.1)
  else clearHsMesh()
})

watch(() => props.zScale, zs => {
  if (zs !== undefined && hsMesh && cachedField) {
    applyZScaleInPlace(zs)
  }
})

watch(() => props.viewMode, mode => {
  applyViewMode(mode)
})

watch(isLight, () => {
  renderer?.setClearColor(clearColor(), 1)
})

onMounted(() => {
  initThree()
  if (props.glbUrl)      loadGlb(props.glbUrl)
  if (props.voxelData)   buildVoxels(props.voxelData)
  if (props.hsFieldData) buildHsField(props.hsFieldData, props.zScale ?? 0.1)
})

onBeforeUnmount(() => {
  if (animId !== null) cancelAnimationFrame(animId)
  ro?.disconnect()
  if (renderer) {
    const el = renderer.domElement
    el.removeEventListener('pointerdown', onHsPointerDown)
    el.removeEventListener('pointermove', onHsPointerMove)
    el.removeEventListener('pointerup',   onHsPointerUp)
    el.removeEventListener('wheel',       onHsWheel)
  }
  controls?.dispose()
  renderer?.dispose()
  clearMesh()
  clearVoxels()
  clearHsMesh()
})
</script>

<template>
  <div class="viewer-wrap">
    <canvas ref="canvasEl" class="canvas" />
    <div v-if="loading" class="overlay">
      <span class="spinner">{{ lang === 'en' ? 'computing…' : '计算中…' }}</span>
    </div>
    <div v-if="!glbUrl && !voxelData && !hsFieldData && !loading" class="overlay">
      <span class="hint">{{ lang === 'en' ? 'no mesh — select mode and compute' : '暂无网格 — 请选择模式并计算' }}</span>
    </div>
  </div>
</template>

<style scoped>
.viewer-wrap {
  position: relative; width: 100%; height: 100%;
  background: var(--bg); overflow: hidden;
}
.canvas { display: block; width: 100%; height: 100%; }
.overlay {
  position: absolute; inset: 0;
  display: flex; align-items: center; justify-content: center;
  pointer-events: none;
}
.spinner, .hint {
  font-family: var(--mono); font-size: 11px;
  color: var(--text-dim); text-transform: uppercase; letter-spacing: 0.08em;
}
.spinner::before { content: '⬡ '; color: var(--accent); }
</style>
