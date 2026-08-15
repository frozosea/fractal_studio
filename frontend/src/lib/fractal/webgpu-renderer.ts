import type { LocalRenderSpec, LocalVariant } from "./local-render-core";

// WebGPU is not part of the TypeScript DOM library used by this project. Keep
// the small surface needed by this module local instead of adding ambient
// declarations (or forcing every browser build to depend on @webgpu/types).
interface WebGpuBuffer {
  destroy(): void;
  getMappedRange(): ArrayBuffer;
  mapAsync(mode: number): Promise<void>;
  unmap(): void;
}

interface WebGpuShaderModule {}
interface WebGpuBindGroupLayout {}
interface WebGpuBindGroup {}
interface WebGpuCommandBuffer {}

interface WebGpuComputePipeline {
  getBindGroupLayout(index: number): WebGpuBindGroupLayout;
}

interface WebGpuComputePass {
  dispatchWorkgroups(x: number, y?: number, z?: number): void;
  end(): void;
  setBindGroup(index: number, bindGroup: WebGpuBindGroup): void;
  setPipeline(pipeline: WebGpuComputePipeline): void;
}

interface WebGpuCommandEncoder {
  beginComputePass(): WebGpuComputePass;
  copyBufferToBuffer(
    source: WebGpuBuffer,
    sourceOffset: number,
    destination: WebGpuBuffer,
    destinationOffset: number,
    size: number,
  ): void;
  finish(): WebGpuCommandBuffer;
}

interface WebGpuQueue {
  submit(commandBuffers: readonly WebGpuCommandBuffer[]): void;
  writeBuffer(buffer: WebGpuBuffer, bufferOffset: number, data: ArrayBufferView): void;
}

interface WebGpuLimits {
  readonly maxBufferSize?: number;
  readonly maxComputeWorkgroupsPerDimension?: number;
  readonly maxStorageBufferBindingSize?: number;
}

interface WebGpuDevice {
  readonly limits: WebGpuLimits;
  readonly lost: Promise<{ readonly message?: string; readonly reason?: string }>;
  readonly queue: WebGpuQueue;
  createBindGroup(descriptor: {
    layout: WebGpuBindGroupLayout;
    entries: Array<{ binding: number; resource: { buffer: WebGpuBuffer } }>;
  }): WebGpuBindGroup;
  createBuffer(descriptor: { size: number; usage: number }): WebGpuBuffer;
  createCommandEncoder(): WebGpuCommandEncoder;
  createComputePipeline(descriptor: {
    layout: "auto";
    compute: { module: WebGpuShaderModule; entryPoint: string };
  }): WebGpuComputePipeline;
  createComputePipelineAsync?(descriptor: {
    layout: "auto";
    compute: { module: WebGpuShaderModule; entryPoint: string };
  }): Promise<WebGpuComputePipeline>;
  createShaderModule(descriptor: { code: string }): WebGpuShaderModule;
  destroy?(): void;
}

interface WebGpuAdapter {
  requestDevice(): Promise<WebGpuDevice>;
}

interface WebGpuEntryPoint {
  requestAdapter(options?: { powerPreference?: "high-performance" | "low-power" }): Promise<WebGpuAdapter | null>;
}

const GPU_BUFFER_USAGE = {
  mapRead: 0x0001,
  copySource: 0x0004,
  copyDestination: 0x0008,
  uniform: 0x0040,
  storage: 0x0080,
} as const;

const GPU_MAP_READ = 0x0001;
const WORKGROUP_SIZE = 8;
const MAX_DIMENSION = 8192;
const MAX_PIXELS = 4_194_304;
const MAX_ITERATIONS = 2048;
const MAX_BAILOUT = 1_000_000;

const VARIANT_INDEX: Partial<Record<LocalVariant, number>> = {
  mandelbrot: 0,
  tricorn: 1,
  burning_ship: 2,
  celtic: 3,
  heart: 4,
  buffalo: 5,
  perp_buffalo: 6,
  celtic_ship: 7,
  mandelceltic: 8,
  perp_ship: 9,
};

const METRIC_INDEX: Partial<Record<LocalRenderSpec["metric"], number>> = {
  escape: 0,
  min_abs: 1,
  max_abs: 2,
  envelope: 3,
};

const PALETTE_INDEX: Readonly<Record<string, number>> = {
  classic_cos: 0,
  mod17: 1,
  hsv_wheel: 2,
  tri765: 3,
  grayscale: 4,
  hs_rainbow: 5,
  inferno: 6,
  viridis: 7,
  twilight: 8,
  ember_blue: 9,
  spectral1530: 10,
};

export type WebGpuIneligibilityReason =
  | "webgpu_unavailable"
  | "unsupported_variant"
  | "unsupported_metric"
  | "unsupported_color_mode"
  | "unsupported_palette"
  | "custom_orbit_program"
  | "custom_color_program"
  | "transition_kernel"
  | "invalid_dimensions"
  | "render_too_large"
  | "iteration_limit"
  | "invalid_numeric_parameter"
  | "f32_precision_insufficient";

export type WebGpuRenderEligibility =
  | { readonly eligible: true }
  | { readonly eligible: false; readonly reason: WebGpuIneligibilityReason };

let devicePromise: Promise<WebGpuDevice | null> | null = null;
let activeDevice: WebGpuDevice | null = null;
let pipelineCache: {
  readonly device: WebGpuDevice;
  readonly promise: Promise<WebGpuComputePipeline>;
} | null = null;

function webGpuEntryPoint(): WebGpuEntryPoint | null {
  if (typeof navigator === "undefined") return null;
  return (navigator as Navigator & { readonly gpu?: WebGpuEntryPoint }).gpu ?? null;
}

function f32SpacingAtMagnitude(magnitude: number): number {
  const rounded = Math.abs(Math.fround(magnitude));
  if (rounded === 0 || rounded < 2 ** -126) return 2 ** -149;
  return 2 ** (Math.floor(Math.log2(rounded)) - 23);
}

function viewportIsResolvable(spec: LocalRenderSpec, width: number, height: number): boolean {
  const angle = spec.rotationDeg * Math.PI / 180;
  const cosine = Math.abs(Math.cos(angle));
  const sine = Math.abs(Math.sin(angle));
  const aspect = width / height;
  const halfScale = spec.scale * 0.5;
  const maximumRe = Math.abs(spec.centerRe) + halfScale * (aspect * cosine + sine);
  const maximumIm = Math.abs(spec.centerIm) + halfScale * (aspect * sine + cosine);
  const reSpacing = f32SpacingAtMagnitude(maximumRe);
  const imSpacing = f32SpacingAtMagnitude(maximumIm);
  const pixelStep = spec.scale / height;
  // Requiring two ULPs prevents long stripes of adjacent pixels collapsing to
  // the same coordinate. The CPU fp64 worker remains available below this
  // threshold.
  const horizontalResolvable = pixelStep * cosine >= 2 * reSpacing
    || pixelStep * sine >= 2 * imSpacing;
  const verticalResolvable = pixelStep * sine >= 2 * reSpacing
    || pixelStep * cosine >= 2 * imSpacing;
  return horizontalResolvable && verticalResolvable;
}

function finiteF32(value: number): boolean {
  return Number.isFinite(value) && Number.isFinite(Math.fround(value));
}

/** Returns a stable reason when a request must remain on the fp64/server path. */
export function webGpuRenderEligibility(
  spec: LocalRenderSpec,
  width: number,
  height: number,
): WebGpuRenderEligibility {
  if (!webGpuEntryPoint()) return { eligible: false, reason: "webgpu_unavailable" };
  if (VARIANT_INDEX[spec.variant] === undefined) return { eligible: false, reason: "unsupported_variant" };
  if (METRIC_INDEX[spec.metric] === undefined) return { eligible: false, reason: "unsupported_metric" };
  if (spec.colorMode !== "direct") return { eligible: false, reason: "unsupported_color_mode" };
  if (PALETTE_INDEX[spec.colorMap] === undefined) return { eligible: false, reason: "unsupported_palette" };
  if (spec.orbitProgram) return { eligible: false, reason: "custom_orbit_program" };
  if (spec.colorProgram) return { eligible: false, reason: "custom_color_program" };
  if (spec.transition) return { eligible: false, reason: "transition_kernel" };
  if (!Number.isInteger(width) || !Number.isInteger(height) || width < 1 || height < 1
    || width > MAX_DIMENSION || height > MAX_DIMENSION) {
    return { eligible: false, reason: "invalid_dimensions" };
  }
  if (width * height > MAX_PIXELS) return { eligible: false, reason: "render_too_large" };
  if (!Number.isInteger(spec.iterations) || spec.iterations < 1 || spec.iterations > MAX_ITERATIONS) {
    return { eligible: false, reason: "iteration_limit" };
  }
  const numericParameters = [
    spec.centerRe,
    spec.centerIm,
    spec.scale,
    spec.rotationDeg,
    spec.bailout,
    ...(spec.julia ? [spec.juliaRe, spec.juliaIm] : []),
  ];
  if (!numericParameters.every(finiteF32) || spec.scale <= 0
    || spec.bailout <= 1 || spec.bailout > MAX_BAILOUT
    || !finiteF32(spec.bailout * spec.bailout)) {
    return { eligible: false, reason: "invalid_numeric_parameter" };
  }
  if (!viewportIsResolvable(spec, width, height)) {
    return { eligible: false, reason: "f32_precision_insufficient" };
  }
  return { eligible: true };
}

export function canRenderWithWebGpu(spec: LocalRenderSpec, width: number, height: number): boolean {
  return webGpuRenderEligibility(spec, width, height).eligible;
}

function abortError(): Error {
  if (typeof DOMException !== "undefined") return new DOMException("Render aborted", "AbortError");
  const error = new Error("Render aborted");
  error.name = "AbortError";
  return error;
}

function isAbortError(error: unknown): boolean {
  return error instanceof Error && error.name === "AbortError";
}

async function abortable<T>(operation: Promise<T>, signal?: AbortSignal): Promise<T> {
  if (!signal) return operation;
  if (signal.aborted) throw abortError();
  let abort: (() => void) | null = null;
  const cancellation = new Promise<never>((_resolve, reject) => {
    abort = () => reject(abortError());
    signal.addEventListener("abort", abort, { once: true });
  });
  try {
    return await Promise.race([operation, cancellation]);
  } finally {
    if (abort) signal.removeEventListener("abort", abort);
  }
}

async function acquireDevice(): Promise<WebGpuDevice | null> {
  if (devicePromise) return devicePromise;
  const gpu = webGpuEntryPoint();
  if (!gpu) return null;
  const request = (async () => {
    try {
      const adapter = await gpu.requestAdapter({ powerPreference: "high-performance" });
      if (!adapter) return null;
      const device = await adapter.requestDevice();
      activeDevice = device;
      void device.lost.then(() => {
        if (activeDevice !== device) return;
        activeDevice = null;
        devicePromise = null;
        if (pipelineCache?.device === device) pipelineCache = null;
      });
      return device;
    } catch {
      return null;
    }
  })();
  devicePromise = request;
  const result = await request;
  if (!result && devicePromise === request) devicePromise = null;
  return result;
}

function retireDevice(device: WebGpuDevice): void {
  if (activeDevice === device) {
    activeDevice = null;
    devicePromise = null;
  }
  if (pipelineCache?.device === device) pipelineCache = null;
  try { device.destroy?.(); } catch { /* The device may already be lost. */ }
}

async function acquirePipeline(device: WebGpuDevice): Promise<WebGpuComputePipeline> {
  if (pipelineCache?.device === device) return pipelineCache.promise;
  const promise = (async () => {
    const module = device.createShaderModule({ code: FRACTAL_SHADER });
    const descriptor = { layout: "auto" as const, compute: { module, entryPoint: "main" } };
    return device.createComputePipelineAsync
      ? device.createComputePipelineAsync(descriptor)
      : device.createComputePipeline(descriptor);
  })();
  pipelineCache = { device, promise };
  try {
    return await promise;
  } catch (error) {
    if (pipelineCache?.promise === promise) pipelineCache = null;
    throw error;
  }
}

function fitsDeviceLimits(device: WebGpuDevice, width: number, height: number): boolean {
  const byteSize = width * height * 4;
  const maximumBuffer = device.limits.maxBufferSize ?? Number.POSITIVE_INFINITY;
  const maximumStorage = device.limits.maxStorageBufferBindingSize ?? Number.POSITIVE_INFINITY;
  const maximumWorkgroups = device.limits.maxComputeWorkgroupsPerDimension ?? 65_535;
  return byteSize <= maximumBuffer
    && byteSize <= maximumStorage
    && Math.ceil(width / WORKGROUP_SIZE) <= maximumWorkgroups
    && Math.ceil(height / WORKGROUP_SIZE) <= maximumWorkgroups;
}

function uniformData(spec: LocalRenderSpec, width: number, height: number): Uint8Array {
  const buffer = new ArrayBuffer(80);
  const view = new DataView(buffer);
  const angle = spec.rotationDeg * Math.PI / 180;
  const variant = VARIANT_INDEX[spec.variant];
  const metric = METRIC_INDEX[spec.metric];
  const palette = PALETTE_INDEX[spec.colorMap];
  if (variant === undefined || metric === undefined || palette === undefined) {
    throw new Error("WebGPU uniform requested for an unsupported render specification");
  }
  const floats = [
    spec.centerRe, spec.centerIm, spec.scale, width / height,
    Math.cos(angle), Math.sin(angle), spec.bailout, spec.bailout * spec.bailout,
    spec.juliaRe, spec.juliaIm, 0, 0,
  ];
  floats.forEach((value, index) => view.setFloat32(index * 4, value, true));
  const integers = [
    width, height, spec.iterations, variant,
    metric, palette, spec.smooth ? 1 : 0, spec.julia ? 1 : 0,
  ];
  integers.forEach((value, index) => view.setUint32(48 + index * 4, value, true));
  return new Uint8Array(buffer);
}

async function renderOnDevice(
  device: WebGpuDevice,
  spec: LocalRenderSpec,
  width: number,
  height: number,
  signal?: AbortSignal,
): Promise<Uint8ClampedArray> {
  if (!fitsDeviceLimits(device, width, height)) throw new Error("WebGPU device limits exceeded");
  if (signal?.aborted) throw abortError();
  const pipeline = await abortable(acquirePipeline(device), signal);
  const byteSize = width * height * 4;
  const parameterBuffer = device.createBuffer({
    size: 80,
    usage: GPU_BUFFER_USAGE.uniform | GPU_BUFFER_USAGE.copyDestination,
  });
  const pixelBuffer = device.createBuffer({
    size: byteSize,
    usage: GPU_BUFFER_USAGE.storage | GPU_BUFFER_USAGE.copySource,
  });
  const readbackBuffer = device.createBuffer({
    size: byteSize,
    usage: GPU_BUFFER_USAGE.mapRead | GPU_BUFFER_USAGE.copyDestination,
  });
  let mapped = false;
  try {
    device.queue.writeBuffer(parameterBuffer, 0, uniformData(spec, width, height));
    const bindGroup = device.createBindGroup({
      layout: pipeline.getBindGroupLayout(0),
      entries: [
        { binding: 0, resource: { buffer: parameterBuffer } },
        { binding: 1, resource: { buffer: pixelBuffer } },
      ],
    });
    const encoder = device.createCommandEncoder();
    const pass = encoder.beginComputePass();
    pass.setPipeline(pipeline);
    pass.setBindGroup(0, bindGroup);
    pass.dispatchWorkgroups(Math.ceil(width / WORKGROUP_SIZE), Math.ceil(height / WORKGROUP_SIZE));
    pass.end();
    encoder.copyBufferToBuffer(pixelBuffer, 0, readbackBuffer, 0, byteSize);
    device.queue.submit([encoder.finish()]);
    await abortable(readbackBuffer.mapAsync(GPU_MAP_READ), signal);
    mapped = true;
    const packed = new Uint32Array(readbackBuffer.getMappedRange());
    const rgba = new Uint8ClampedArray(byteSize);
    for (let index = 0; index < packed.length; index += 1) {
      const color = packed[index] ?? 0xff000000;
      const offset = index * 4;
      rgba[offset] = color & 0xff;
      rgba[offset + 1] = (color >>> 8) & 0xff;
      rgba[offset + 2] = (color >>> 16) & 0xff;
      rgba[offset + 3] = 255;
    }
    return rgba;
  } finally {
    if (mapped) {
      try { readbackBuffer.unmap(); } catch { /* A lost device can unmap it first. */ }
    }
    parameterBuffer.destroy();
    pixelBuffer.destroy();
    readbackBuffer.destroy();
  }
}

/**
 * Renders an f32 preview, or returns null so the caller can use fp64/server
 * rendering. Aborts reject with AbortError; device loss is retried once with a
 * freshly requested adapter/device.
 */
export async function renderWebGpuRgba(
  spec: LocalRenderSpec,
  width: number,
  height: number,
  signal?: AbortSignal,
): Promise<Uint8ClampedArray | null> {
  if (!webGpuRenderEligibility(spec, width, height).eligible) return null;
  for (let attempt = 0; attempt < 2; attempt += 1) {
    const device = await abortable(acquireDevice(), signal);
    if (!device) return null;
    try {
      return await renderOnDevice(device, spec, width, height, signal);
    } catch (error) {
      if (isAbortError(error)) throw error;
      retireDevice(device);
    }
  }
  return null;
}

const FRACTAL_SHADER = /* wgsl */ `
struct Parameters {
  viewport: vec4<f32>,
  rotation: vec4<f32>,
  julia: vec4<f32>,
  dimensions: vec4<u32>,
  modes: vec4<u32>,
}

struct Sample {
  iteration: u32,
  norm: f32,
  field: f32,
}

@group(0) @binding(0) var<uniform> parameters: Parameters;
@group(0) @binding(1) var<storage, read_write> pixels: array<u32>;

fn byte_value(value: f32) -> u32 {
  return u32(clamp(value, 0.0, 255.0));
}

fn rounded_rgb(value: vec3<f32>) -> vec3<u32> {
  return vec3<u32>(
    u32(clamp(floor(value.x + 0.5), 0.0, 255.0)),
    u32(clamp(floor(value.y + 0.5), 0.0, 255.0)),
    u32(clamp(floor(value.z + 0.5), 0.0, 255.0))
  );
}

fn pack_rgb(value: vec3<u32>) -> u32 {
  return value.x | (value.y << 8u) | (value.z << 16u) | 0xff000000u;
}

fn mix_rgb(t: f32, left_t: f32, right_t: f32, left: vec3<f32>, right: vec3<f32>) -> vec3<u32> {
  let u = (t - left_t) / max(1e-12, right_t - left_t);
  return rounded_rgb(left * (1.0 - u) + right * u);
}

// The alpha-like fourth channel is a handled flag, not an output alpha value.
fn gradient_color(palette: u32, input: f32) -> vec4<u32> {
  let t = clamp(input, 0.0, 1.0);
  if (palette == 6u) {
    if (t <= 0.14) { return vec4<u32>(mix_rgb(t, 0.0, 0.14, vec3<f32>(0.0, 0.0, 4.0), vec3<f32>(31.0, 12.0, 72.0)), 1u); }
    if (t <= 0.28) { return vec4<u32>(mix_rgb(t, 0.14, 0.28, vec3<f32>(31.0, 12.0, 72.0), vec3<f32>(85.0, 15.0, 109.0)), 1u); }
    if (t <= 0.42) { return vec4<u32>(mix_rgb(t, 0.28, 0.42, vec3<f32>(85.0, 15.0, 109.0), vec3<f32>(136.0, 34.0, 106.0)), 1u); }
    if (t <= 0.56) { return vec4<u32>(mix_rgb(t, 0.42, 0.56, vec3<f32>(136.0, 34.0, 106.0), vec3<f32>(186.0, 54.0, 85.0)), 1u); }
    if (t <= 0.70) { return vec4<u32>(mix_rgb(t, 0.56, 0.70, vec3<f32>(186.0, 54.0, 85.0), vec3<f32>(227.0, 89.0, 51.0)), 1u); }
    if (t <= 0.84) { return vec4<u32>(mix_rgb(t, 0.70, 0.84, vec3<f32>(227.0, 89.0, 51.0), vec3<f32>(249.0, 140.0, 10.0)), 1u); }
    if (t <= 0.94) { return vec4<u32>(mix_rgb(t, 0.84, 0.94, vec3<f32>(249.0, 140.0, 10.0), vec3<f32>(252.0, 195.0, 55.0)), 1u); }
    return vec4<u32>(mix_rgb(t, 0.94, 1.0, vec3<f32>(252.0, 195.0, 55.0), vec3<f32>(252.0, 255.0, 164.0)), 1u);
  }
  if (palette == 7u) {
    if (t <= 0.25) { return vec4<u32>(mix_rgb(t, 0.0, 0.25, vec3<f32>(68.0, 1.0, 84.0), vec3<f32>(59.0, 82.0, 139.0)), 1u); }
    if (t <= 0.50) { return vec4<u32>(mix_rgb(t, 0.25, 0.50, vec3<f32>(59.0, 82.0, 139.0), vec3<f32>(33.0, 145.0, 140.0)), 1u); }
    if (t <= 0.75) { return vec4<u32>(mix_rgb(t, 0.50, 0.75, vec3<f32>(33.0, 145.0, 140.0), vec3<f32>(94.0, 201.0, 98.0)), 1u); }
    return vec4<u32>(mix_rgb(t, 0.75, 1.0, vec3<f32>(94.0, 201.0, 98.0), vec3<f32>(253.0, 231.0, 37.0)), 1u);
  }
  if (palette == 8u) {
    if (t <= 0.18) { return vec4<u32>(mix_rgb(t, 0.0, 0.18, vec3<f32>(32.0, 24.0, 70.0), vec3<f32>(63.0, 92.0, 180.0)), 1u); }
    if (t <= 0.36) { return vec4<u32>(mix_rgb(t, 0.18, 0.36, vec3<f32>(63.0, 92.0, 180.0), vec3<f32>(58.0, 150.0, 165.0)), 1u); }
    if (t <= 0.54) { return vec4<u32>(mix_rgb(t, 0.36, 0.54, vec3<f32>(58.0, 150.0, 165.0), vec3<f32>(240.0, 210.0, 120.0)), 1u); }
    if (t <= 0.72) { return vec4<u32>(mix_rgb(t, 0.54, 0.72, vec3<f32>(240.0, 210.0, 120.0), vec3<f32>(210.0, 90.0, 90.0)), 1u); }
    if (t <= 0.88) { return vec4<u32>(mix_rgb(t, 0.72, 0.88, vec3<f32>(210.0, 90.0, 90.0), vec3<f32>(90.0, 50.0, 110.0)), 1u); }
    return vec4<u32>(mix_rgb(t, 0.88, 1.0, vec3<f32>(90.0, 50.0, 110.0), vec3<f32>(32.0, 24.0, 70.0)), 1u);
  }
  if (palette == 9u) {
    if (t <= 0.22) { return vec4<u32>(mix_rgb(t, 0.0, 0.22, vec3<f32>(5.0, 8.0, 32.0), vec3<f32>(10.0, 70.0, 120.0)), 1u); }
    if (t <= 0.48) { return vec4<u32>(mix_rgb(t, 0.22, 0.48, vec3<f32>(10.0, 70.0, 120.0), vec3<f32>(55.0, 190.0, 185.0)), 1u); }
    if (t <= 0.72) { return vec4<u32>(mix_rgb(t, 0.48, 0.72, vec3<f32>(55.0, 190.0, 185.0), vec3<f32>(245.0, 172.0, 75.0)), 1u); }
    return vec4<u32>(mix_rgb(t, 0.72, 1.0, vec3<f32>(245.0, 172.0, 75.0), vec3<f32>(255.0, 246.0, 210.0)), 1u);
  }
  return vec4<u32>(0u, 0u, 0u, 0u);
}

fn hsv(hue: f32) -> vec3<u32> {
  let h = (hue / 360.0 - floor(hue / 360.0)) * 6.0;
  let x = 1.0 - abs((h - 2.0 * floor(h * 0.5)) - 1.0);
  var color = vec3<f32>(1.0, 0.0, 0.0);
  if (h < 1.0) { color = vec3<f32>(1.0, x, 0.0); }
  else if (h < 2.0) { color = vec3<f32>(x, 1.0, 0.0); }
  else if (h < 3.0) { color = vec3<f32>(0.0, 1.0, x); }
  else if (h < 4.0) { color = vec3<f32>(0.0, x, 1.0); }
  else if (h < 5.0) { color = vec3<f32>(x, 0.0, 1.0); }
  else { color = vec3<f32>(1.0, 0.0, x); }
  return vec3<u32>(byte_value(color.x * 255.0), byte_value(color.y * 255.0), byte_value(color.z * 255.0));
}

fn hue1530(input: u32) -> vec3<u32> {
  let i = input % 1530u;
  let segment = i / 255u;
  let d = i % 255u;
  if (segment == 0u) { return vec3<u32>(0u, 255u, d); }
  if (segment == 1u) { return vec3<u32>(0u, 255u - d, 255u); }
  if (segment == 2u) { return vec3<u32>(d, 0u, 255u); }
  if (segment == 3u) { return vec3<u32>(255u, 0u, 255u - d); }
  if (segment == 4u) { return vec3<u32>(255u, d, 0u); }
  return vec3<u32>(255u - d, 255u, 0u);
}

fn tri765(input: u32) -> vec3<u32> {
  let i = input % 765u;
  let band = i / 255u;
  let d = i % 255u;
  if (band == 0u) { return vec3<u32>(255u - d, d, 255u); }
  if (band == 1u) { return vec3<u32>(d, 255u, 255u - d); }
  return vec3<u32>(255u, 255u - d, d);
}

fn rainbow1785(input: f32) -> vec3<u32> {
  let i = u32(floor(clamp(input, 0.0, 1785.0)));
  if (i == 0u) { return vec3<u32>(0u); }
  if (i == 1785u) { return vec3<u32>(255u); }
  var red = 0u;
  var green = 0u;
  var blue = i;
  if (i > 255u && i < 510u) { red = i - 255u; blue = 510u - i; }
  else if (i > 509u && i < 765u) { red = 255u; blue = i - 510u; }
  else if (i > 764u && i < 1020u) { green = i - 765u; red = 1020u - i; blue = red; }
  else if (i > 1019u && i < 1275u) { green = 255u; blue = i - 1020u; }
  else if (i > 1274u && i < 1530u) { green = 255u; red = i - 1275u; blue = 1530u - i; }
  else if (i > 1529u) { green = 255u; red = 255u; blue = i - 1530u; }
  return vec3<u32>(min(red, 255u), min(green, 255u), min(blue, 255u));
}

fn field_color(value: f32, palette: u32) -> vec3<u32> {
  var t = 1.0;
  if (value == value && abs(value) <= 3.402823e38) { t = clamp(value, 0.0, 1.0); }
  let gradient = gradient_color(palette, t);
  if (gradient.w == 1u) { return gradient.xyz; }
  if (palette == 4u) { let channel = byte_value(t * 255.0); return vec3<u32>(channel); }
  if (palette == 2u) { return hsv(t * 360.0); }
  if (palette == 3u) { return tri765(u32(floor(t * 765.0))); }
  if (palette == 5u) { return rainbow1785(t * 1785.0); }
  if (palette == 10u) { return hue1530(min(1529u, u32(floor(t * 1530.0)))); }
  if (palette == 1u) { let channel = min(16u, u32(floor(t * 17.0))) * 15u; return vec3<u32>(channel); }
  let tau = 6.283185307179586;
  return vec3<u32>(
    byte_value(128.0 - 128.0 * cos(t * tau)),
    byte_value(128.0 - 128.0 * cos(t * tau + 2.094395)),
    byte_value(128.0 - 128.0 * cos(t * tau + 4.18879))
  );
}

fn orbit_step(variant: u32, z: vec2<f32>, c: vec2<f32>) -> vec2<f32> {
  let x2 = z.x * z.x;
  let y2 = z.y * z.y;
  let xy2 = 2.0 * z.x * z.y;
  if (variant == 0u) { return vec2<f32>(x2 - y2 + c.x, xy2 + c.y); }
  if (variant == 1u) { return vec2<f32>(x2 - y2 + c.x, -xy2 + c.y); }
  if (variant == 2u) { return vec2<f32>(x2 - y2 + c.x, abs(xy2) + c.y); }
  if (variant == 3u) { return vec2<f32>(x2 - y2 + c.x, 2.0 * z.x * abs(z.y) + c.y); }
  if (variant == 4u) { return vec2<f32>(x2 - y2 + c.x, -2.0 * abs(z.x) * z.y + c.y); }
  if (variant == 5u) { return vec2<f32>(abs(x2 - y2) + c.x, xy2 + c.y); }
  if (variant == 6u) { return vec2<f32>(abs(x2 - y2) + c.x, -xy2 + c.y); }
  if (variant == 7u) { return vec2<f32>(abs(x2 - y2) + c.x, abs(xy2) + c.y); }
  if (variant == 8u) { return vec2<f32>(abs(x2 - y2) + c.x, 2.0 * z.x * abs(z.y) + c.y); }
  return vec2<f32>(abs(x2 - y2) + c.x, -2.0 * abs(z.x) * z.y + c.y);
}

fn field_value(metric: u32, minimum_squared: f32, maximum_squared: f32) -> f32 {
  var minimum = 0.0;
  if (minimum_squared <= 3.402823e38) { minimum = sqrt(max(0.0, minimum_squared)); }
  var maximum = 0.0;
  if (maximum_squared > 0.0) { maximum = sqrt(maximum_squared); }
  if (metric == 1u) { return minimum; }
  if (metric == 2u) { return maximum; }
  if (metric == 3u) { return 0.5 * (minimum + maximum); }
  return 0.0;
}

fn iterate(point: vec2<f32>) -> Sample {
  let julia_mode = parameters.modes.w == 1u;
  var z = select(vec2<f32>(0.0), point, julia_mode);
  let c = select(point, parameters.julia.xy, julia_mode);
  var minimum = 3.402823e38;
  var maximum = 0.0;
  for (var iteration = 0u; iteration < parameters.dimensions.z; iteration += 1u) {
    z = orbit_step(parameters.dimensions.w, z, c);
    let norm = dot(z, z);
    minimum = min(minimum, norm);
    maximum = max(maximum, norm);
    if (!(norm <= 3.402823e38)) {
      return Sample(iteration, 0.0, field_value(parameters.modes.x, minimum, maximum));
    }
    if (norm > parameters.rotation.w) {
      return Sample(iteration, norm, field_value(parameters.modes.x, minimum, maximum));
    }
  }
  return Sample(parameters.dimensions.z, 0.0, field_value(parameters.modes.x, minimum, maximum));
}

fn escape_color(sample: Sample, palette: u32) -> vec3<u32> {
  if (sample.iteration >= parameters.dimensions.z) { return vec3<u32>(255u); }
  var n = f32(sample.iteration + 1u) / f32(parameters.dimensions.z + 2u);
  var mu = f32(sample.iteration);
  if (parameters.modes.z == 1u && sample.norm > 1.0) {
    mu = max(0.0, f32(sample.iteration + 1u) - log2(log2(sample.norm)));
    n = fract(mu / 32.0);
  }
  let gradient = gradient_color(palette, n);
  if (gradient.w == 1u) { return gradient.xyz; }
  if (palette == 2u) {
    if (parameters.modes.z == 1u) { return hsv(n * 360.0); }
    return hsv(f32(sample.iteration % 1440u) / 4.0);
  }
  if (palette == 3u) {
    if (parameters.modes.z == 1u) { return tri765(u32(floor(n * 765.0))); }
    return tri765(sample.iteration);
  }
  if (palette == 4u) { let channel = byte_value(n * 255.0); return vec3<u32>(channel); }
  if (palette == 10u) {
    if (mu < 255.0) { return vec3<u32>(0u, byte_value(mu), 0u); }
    return hue1530(u32(floor(mu - 255.0)));
  }
  if (palette == 1u) {
    if (parameters.modes.z == 1u) { let channel = (u32(floor(mu)) % 17u) * 15u; return vec3<u32>(channel); }
    return vec3<u32>(sample.iteration % 256u, sample.iteration / 256u, (sample.iteration % 17u) * 17u);
  }
  let pi = 3.141592653589793;
  return vec3<u32>(
    byte_value(128.0 - 128.0 * cos(n * 53.0 * pi)),
    byte_value(128.0 - 128.0 * cos(n * 27.0 * pi)),
    byte_value(128.0 - 128.0 * cos(n * 139.0 * pi))
  );
}

fn metric_color(sample: Sample, palette: u32) -> vec3<u32> {
  let raw = sample.field;
  if (palette == 5u) {
    if (raw <= 0.0 || !(raw <= 3.402823e38)) { return vec3<u32>(255u); }
    return rainbow1785((36.0 / 35.0 - log2(raw)) * 35.0);
  }
  if (parameters.modes.z == 1u) {
    if (raw <= 0.0) { return vec3<u32>(255u); }
    let base = 2.0 - log2(raw);
    let cycle = fract(base / 8.0);
    let gradient = gradient_color(palette, cycle);
    if (gradient.w == 1u) { return gradient.xyz; }
    if (palette == 2u) { return hsv(f32(u32(floor(max(0.0, 180.0 * base))) % 1440u) / 4.0); }
    if (palette == 3u) { return tri765(u32(floor(max(0.0, 96.0 * base)))); }
    if (palette == 10u) { return hue1530(u32(floor(max(0.0, 191.0 * base)))); }
    if (palette == 4u) { let channel = u32(floor(max(0.0, 32.0 * base))) % 256u; return vec3<u32>(channel); }
  }
  return field_color(raw / parameters.rotation.z, palette);
}

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) id: vec3<u32>) {
  let width = parameters.dimensions.x;
  let height = parameters.dimensions.y;
  if (id.x >= width || id.y >= height) { return; }
  let local_re = ((f32(id.x) + 0.5) / f32(width) - 0.5) * parameters.viewport.z * parameters.viewport.w;
  let local_im = (0.5 - (f32(id.y) + 0.5) / f32(height)) * parameters.viewport.z;
  let point = parameters.viewport.xy + vec2<f32>(
    local_re * parameters.rotation.x - local_im * parameters.rotation.y,
    local_re * parameters.rotation.y + local_im * parameters.rotation.x
  );
  let sample = iterate(point);
  var color = escape_color(sample, parameters.modes.y);
  if (parameters.modes.x != 0u) { color = metric_color(sample, parameters.modes.y); }
  pixels[id.y * width + id.x] = pack_rgb(color);
}
`;
