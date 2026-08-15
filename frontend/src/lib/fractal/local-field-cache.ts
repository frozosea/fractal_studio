export type RawNormArray = Float32Array | Float64Array;

type RawFieldBase = {
  readonly width: number;
  readonly height: number;
  readonly bailout: number;
};

export type RawEscapeField = RawFieldBase & {
  readonly kind: "escape";
  readonly metric: "escape";
  readonly iterationLimit: number;
  readonly iterations: Uint32Array;
  readonly norms: RawNormArray;
};

export type RawMetricField = RawFieldBase & {
  readonly kind: "metric";
  readonly metric: string;
  readonly values: Float64Array;
};

/** Numeric orbit output retained by the worker independently of presentation. */
export type RawField = RawEscapeField | RawMetricField;

export type ColorProgramWrap = "clamp" | "repeat" | "mirror";

export type ColorProgramV1 = {
  readonly schemaVersion: 1;
  readonly type: "gradient";
  readonly interpolation?: "rgb";
  readonly wrap?: ColorProgramWrap;
  readonly cycles?: number;
  readonly phase?: number;
  readonly interiorColor?: string;
  readonly invalidColor?: string;
  readonly stops: ReadonlyArray<{
    readonly at: number;
    readonly color: string;
  }>;
};

export type CompiledColorProgram = {
  readonly kind: "compiled-color-program-v1";
  readonly wrap: ColorProgramWrap;
  readonly cycles: number;
  readonly phase: number;
  /** Strictly increasing positions, including zero and one. */
  readonly positions: Float64Array;
  /** Interleaved RGB triples corresponding to positions. */
  readonly colors: Uint8Array;
  readonly interior: Uint8Array;
  readonly invalid: Uint8Array;
};

export class ColorProgramError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "ColorProgramError";
  }
}

const PROGRAM_FIELDS = new Set([
  "schemaVersion", "type", "interpolation", "wrap", "cycles", "phase",
  "interiorColor", "invalidColor", "stops",
]);
const STOP_FIELDS = new Set(["at", "color"]);
const HEX_COLOR = /^#[0-9a-fA-F]{6}$/;

function record(value: unknown, message: string): Record<string, unknown> {
  if (value === null || typeof value !== "object" || Array.isArray(value)) {
    throw new ColorProgramError(message);
  }
  return value as Record<string, unknown>;
}

function rejectUnknownFields(value: Record<string, unknown>, allowed: ReadonlySet<string>, objectName: string): void {
  for (const name of Object.keys(value)) {
    if (!allowed.has(name)) throw new ColorProgramError(`${objectName} contains unknown field: ${name}`);
  }
}

function parseColor(value: unknown, field: string): readonly [number, number, number] {
  if (typeof value !== "string" || !HEX_COLOR.test(value)) {
    throw new ColorProgramError(`${field} must be #RRGGBB`);
  }
  return [
    Number.parseInt(value.slice(1, 3), 16),
    Number.parseInt(value.slice(3, 5), 16),
    Number.parseInt(value.slice(5, 7), 16),
  ];
}

function optionalFiniteNumber(value: unknown, fallback: number, field: string): number {
  if (value === undefined) return fallback;
  if (typeof value !== "number" || !Number.isFinite(value)) {
    throw new ColorProgramError(`${field} must be finite`);
  }
  return value;
}

/** Parse and validate the same bounded declarative program accepted by the backend. */
export function compileColorProgram(input: unknown): CompiledColorProgram {
  const value = record(input, "colorProgram must be an object");
  rejectUnknownFields(value, PROGRAM_FIELDS, "colorProgram");
  if (value.schemaVersion !== 1) throw new ColorProgramError("colorProgram.schemaVersion must be 1");
  if (value.type !== "gradient") throw new ColorProgramError("colorProgram.type must be gradient");
  if ((value.interpolation ?? "rgb") !== "rgb") {
    throw new ColorProgramError("colorProgram.interpolation must be rgb");
  }

  const wrap = value.wrap ?? "clamp";
  if (wrap !== "clamp" && wrap !== "repeat" && wrap !== "mirror") {
    throw new ColorProgramError("colorProgram.wrap must be clamp, repeat, or mirror");
  }
  const cycles = optionalFiniteNumber(value.cycles, 1, "colorProgram.cycles");
  if (!(cycles > 0) || cycles > 256) {
    throw new ColorProgramError("colorProgram.cycles must be finite and in (0,256]");
  }
  const phase = optionalFiniteNumber(value.phase, 0, "colorProgram.phase");
  const interior = value.interiorColor === undefined
    ? [255, 255, 255] as const
    : parseColor(value.interiorColor, "colorProgram.interiorColor");
  const invalid = value.invalidColor === undefined
    ? [255, 0, 255] as const
    : parseColor(value.invalidColor, "colorProgram.invalidColor");

  if (!Array.isArray(value.stops) || value.stops.length < 2 || value.stops.length > 16) {
    throw new ColorProgramError("colorProgram.stops must contain 2..16 entries");
  }
  const positions = new Float64Array(value.stops.length);
  const colors = new Uint8Array(value.stops.length * 3);
  let previous = -1;
  for (let index = 0; index < value.stops.length; index += 1) {
    const stop = record(value.stops[index], "color stop must be an object");
    rejectUnknownFields(stop, STOP_FIELDS, "color stop");
    if (typeof stop.at !== "number" || stop.color === undefined) {
      throw new ColorProgramError("color stop requires numeric at and #RRGGBB color");
    }
    const at = stop.at;
    if (!Number.isFinite(at) || at < 0 || at > 1 || at <= previous) {
      throw new ColorProgramError("color stop positions must be finite and strictly increasing in [0,1]");
    }
    const color = parseColor(stop.color, "color stop color");
    positions[index] = at;
    colors[index * 3] = color[0];
    colors[index * 3 + 1] = color[1];
    colors[index * 3 + 2] = color[2];
    previous = at;
  }
  if (positions[0] !== 0 || positions[positions.length - 1] !== 1) {
    throw new ColorProgramError("colorProgram stops must start at 0 and end at 1");
  }

  return {
    kind: "compiled-color-program-v1",
    wrap,
    cycles,
    phase,
    positions,
    colors,
    interior: Uint8Array.from(interior),
    invalid: Uint8Array.from(invalid),
  };
}

export function isCompiledColorProgram(value: unknown): value is CompiledColorProgram {
  if (value === null || typeof value !== "object") return false;
  const candidate = value as Partial<CompiledColorProgram>;
  return candidate.kind === "compiled-color-program-v1"
    && candidate.positions instanceof Float64Array
    && candidate.colors instanceof Uint8Array
    && candidate.interior instanceof Uint8Array
    && candidate.invalid instanceof Uint8Array;
}

function assertPositiveInteger(value: number, field: string): void {
  if (!Number.isSafeInteger(value) || value < 1) throw new RangeError(`${field} must be a positive safe integer`);
}

/** Throws when field metadata and its typed-array payload disagree. */
export function assertRawField(field: RawField): void {
  assertPositiveInteger(field.width, "RawField.width");
  assertPositiveInteger(field.height, "RawField.height");
  const pixels = field.width * field.height;
  if (!Number.isSafeInteger(pixels)) throw new RangeError("RawField dimensions are too large");
  if (!Number.isFinite(field.bailout) || field.bailout <= 0) {
    throw new RangeError("RawField.bailout must be finite and positive");
  }
  if (field.kind === "escape") {
    if (field.metric !== "escape") throw new TypeError("escape RawField must use the escape metric");
    assertPositiveInteger(field.iterationLimit, "RawField.iterationLimit");
    if (!(field.iterations instanceof Uint32Array) || field.iterations.length !== pixels) {
      throw new RangeError("escape RawField iterations length does not match its dimensions");
    }
    if (!(field.norms instanceof Float32Array || field.norms instanceof Float64Array)
      || field.norms.length !== pixels) {
      throw new RangeError("escape RawField norms length does not match its dimensions");
    }
    return;
  }
  if (field.kind !== "metric" || field.metric === "escape" || field.metric.length === 0) {
    throw new TypeError("metric RawField must name a non-escape metric");
  }
  if (!(field.values instanceof Float64Array) || field.values.length !== pixels) {
    throw new RangeError("metric RawField values length does not match its dimensions");
  }
}

function retainedArrayBytes(field: RawField): number {
  const views: ArrayBufferView[] = field.kind === "escape"
    ? [field.iterations, field.norms]
    : [field.values];
  const buffers = new Set<ArrayBufferLike>();
  let bytes = 0;
  for (const view of views) {
    if (buffers.has(view.buffer)) continue;
    buffers.add(view.buffer);
    bytes += view.buffer.byteLength;
  }
  return bytes;
}

/** Backing-store bytes retained by a field (shared buffers are counted once). */
export function rawFieldByteLength(field: RawField): number {
  assertRawField(field);
  return retainedArrayBytes(field);
}

const PRESENTATION_FIELDS = new Set([
  "colorMap", "colorMode", "colorProgram", "cyclesPerOctave", "smooth",
  "engine", "scalarType", "renderThreads", "tileSize",
]);

function canonicalEncode(value: unknown, ancestors: Set<object>): string {
  if (value === null) return "null";
  if (value === undefined) return "u";
  if (typeof value === "boolean") return value ? "b:1" : "b:0";
  if (typeof value === "string") return `s:${JSON.stringify(value)}`;
  if (typeof value === "number") {
    if (Number.isNaN(value)) return "n:nan";
    if (value === Number.POSITIVE_INFINITY) return "n:+inf";
    if (value === Number.NEGATIVE_INFINITY) return "n:-inf";
    if (Object.is(value, -0)) return "n:-0";
    return `n:${String(value)}`;
  }
  if (typeof value === "bigint") return `i:${String(value)}`;
  if (typeof value !== "object") throw new TypeError(`cannot canonicalize ${typeof value}`);
  if (ancestors.has(value)) throw new TypeError("cannot canonicalize a cyclic orbit specification");
  ancestors.add(value);
  let result: string;
  if (Array.isArray(value)) {
    result = `a:[${value.map((item) => canonicalEncode(item, ancestors)).join(",")}]`;
  } else {
    const prototype = Object.getPrototypeOf(value);
    if (prototype !== Object.prototype && prototype !== null) {
      ancestors.delete(value);
      throw new TypeError("orbit specification must contain only plain objects and arrays");
    }
    const source = value as Record<string, unknown>;
    result = `o:{${Object.keys(source).sort().map((key) =>
      `${JSON.stringify(key)}:${canonicalEncode(source[key], ancestors)}`).join(",")}}`;
  }
  ancestors.delete(value);
  return result;
}

export const RAW_FIELD_CACHE_KEY_VERSION = "browser-raw-field-v1";

/**
 * Canonical key for a normalized local render spec. Presentation-only fields
 * are deliberately ignored so palette and gradient edits reuse orbit work.
 */
export function createRawFieldCacheKey(
  spec: Readonly<Record<string, unknown>>,
  width: number,
  height: number,
  rendererVersion = RAW_FIELD_CACHE_KEY_VERSION,
): string {
  assertPositiveInteger(width, "width");
  assertPositiveInteger(height, "height");
  if (typeof rendererVersion !== "string" || rendererVersion.length === 0) {
    throw new TypeError("rendererVersion must be a non-empty string");
  }
  const orbitSpec: Record<string, unknown> = Object.create(null) as Record<string, unknown>;
  for (const [key, value] of Object.entries(spec)) {
    if (!PRESENTATION_FIELDS.has(key) && value !== undefined) orbitSpec[key] = value;
  }
  return canonicalEncode({ rendererVersion, width, height, spec: orbitSpec }, new Set());
}

type CacheEntry = {
  readonly field: RawField;
  readonly bytes: number;
};

function checkedByteLimit(value: number): number {
  if (!Number.isSafeInteger(value) || value < 0) {
    throw new RangeError("RawFieldCache maxBytes must be a non-negative safe integer");
  }
  return value;
}

/** Worker-local least-recently-used cache with a conservative backing-store cap. */
export class RawFieldCache {
  private readonly cache = new Map<string, CacheEntry>();
  private byteCount = 0;
  private byteLimit: number;

  constructor(maxBytes = 64 * 1024 * 1024) {
    this.byteLimit = checkedByteLimit(maxBytes);
  }

  get size(): number { return this.cache.size; }
  get bytes(): number { return this.byteCount; }
  get maxBytes(): number { return this.byteLimit; }

  get(key: string): RawField | undefined {
    const entry = this.cache.get(key);
    if (entry === undefined) return undefined;
    this.cache.delete(key);
    this.cache.set(key, entry);
    return entry.field;
  }

  peek(key: string): RawField | undefined {
    return this.cache.get(key)?.field;
  }

  has(key: string): boolean {
    return this.cache.has(key);
  }

  /** Returns false, without replacing an existing entry, when one field exceeds the cap. */
  set(key: string, field: RawField): boolean {
    if (typeof key !== "string" || key.length === 0) throw new TypeError("cache key must be a non-empty string");
    assertRawField(field);
    const bytes = retainedArrayBytes(field);
    if (bytes > this.byteLimit) return false;

    const existing = this.cache.get(key);
    if (existing !== undefined) {
      this.cache.delete(key);
      this.byteCount -= existing.bytes;
    }
    while (this.byteCount + bytes > this.byteLimit) this.evictOldest();
    this.cache.set(key, { field, bytes });
    this.byteCount += bytes;
    return true;
  }

  delete(key: string): boolean {
    const entry = this.cache.get(key);
    if (entry === undefined) return false;
    this.cache.delete(key);
    this.byteCount -= entry.bytes;
    return true;
  }

  clear(): void {
    this.cache.clear();
    this.byteCount = 0;
  }

  resize(maxBytes: number): void {
    this.byteLimit = checkedByteLimit(maxBytes);
    while (this.byteCount > this.byteLimit) this.evictOldest();
  }

  private evictOldest(): void {
    const oldest = this.cache.entries().next().value as [string, CacheEntry] | undefined;
    if (oldest === undefined) return;
    this.cache.delete(oldest[0]);
    this.byteCount -= oldest[1].bytes;
  }
}

export type RawFieldColorizeOptions = {
  readonly smooth?: boolean;
  readonly target?: Uint8ClampedArray;
};

function writeRgb(source: Uint8Array, sourceOffset: number, target: Uint8ClampedArray, targetOffset: number): void {
  target[targetOffset] = source[sourceOffset]!;
  target[targetOffset + 1] = source[sourceOffset + 1]!;
  target[targetOffset + 2] = source[sourceOffset + 2]!;
  target[targetOffset + 3] = 255;
}

function wrapProgramValue(value: number, wrap: ColorProgramWrap): number {
  if (wrap === "clamp") return Math.max(0, Math.min(1, value));
  if (wrap === "repeat") {
    const result = value - Math.floor(value);
    return result < 0 ? result + 1 : result;
  }
  let result = value % 2;
  if (result < 0) result += 2;
  return result <= 1 ? result : 2 - result;
}

function lroundChannel(left: number, right: number, amount: number): number {
  const mixed = left * (1 - amount) + right * amount;
  return Math.max(0, Math.min(255, Math.floor(mixed + 0.5)));
}

function writeProgramColor(
  program: CompiledColorProgram,
  input: number,
  target: Uint8ClampedArray,
  offset: number,
): void {
  if (!Number.isFinite(input)) {
    writeRgb(program.invalid, 0, target, offset);
    return;
  }
  const value = wrapProgramValue(input * program.cycles + program.phase, program.wrap);
  if (value <= program.positions[0]!) {
    writeRgb(program.colors, 0, target, offset);
    return;
  }
  for (let index = 1; index < program.positions.length; index += 1) {
    if (value > program.positions[index]!) continue;
    const leftAt = program.positions[index - 1]!;
    const rightAt = program.positions[index]!;
    const span = Math.max(1e-12, rightAt - leftAt);
    const amount = Math.max(0, Math.min(1, (value - leftAt) / span));
    const left = (index - 1) * 3;
    const right = index * 3;
    target[offset] = lroundChannel(program.colors[left]!, program.colors[right]!, amount);
    target[offset + 1] = lroundChannel(program.colors[left + 1]!, program.colors[right + 1]!, amount);
    target[offset + 2] = lroundChannel(program.colors[left + 2]!, program.colors[right + 2]!, amount);
    target[offset + 3] = 255;
    return;
  }
  writeRgb(program.colors, program.colors.length - 3, target, offset);
}

function smoothMu(iteration: number, norm: number): number {
  if (norm > 1) {
    const mu = iteration + 1 - Math.log2(Math.log2(norm));
    return mu > 0 ? mu : 0;
  }
  return iteration;
}

function normalizeMetric(value: number, bailout: number): number {
  if (value <= 0) return 0;
  return Math.min(1, value / bailout);
}

/**
 * Convert a cached orbit field to browser RGBA using backend ColorProgram v1
 * semantics. Supplying a target avoids allocating during repeated recoloring.
 */
export function colorizeRawField(
  field: RawField,
  colorProgram: ColorProgramV1 | CompiledColorProgram,
  options: RawFieldColorizeOptions = {},
): Uint8ClampedArray {
  assertRawField(field);
  const program = isCompiledColorProgram(colorProgram) ? colorProgram : compileColorProgram(colorProgram);
  const requiredLength = field.width * field.height * 4;
  const rgba = options.target ?? new Uint8ClampedArray(requiredLength);
  if (!(rgba instanceof Uint8ClampedArray) || rgba.length !== requiredLength) {
    throw new RangeError("RGBA target length does not match RawField dimensions");
  }

  if (field.kind === "escape") {
    const smooth = options.smooth === true;
    for (let index = 0; index < field.iterations.length; index += 1) {
      const offset = index * 4;
      const iteration = field.iterations[index]!;
      if (iteration >= field.iterationLimit) {
        writeRgb(program.interior, 0, rgba, offset);
        continue;
      }
      const input = smooth
        ? smoothMu(iteration, field.norms[index]!) / 32
        : (iteration + 1) / (field.iterationLimit + 2);
      writeProgramColor(program, input, rgba, offset);
    }
    return rgba;
  }

  for (let index = 0; index < field.values.length; index += 1) {
    const offset = index * 4;
    const value = field.values[index]!;
    if (!Number.isFinite(value)) {
      writeRgb(program.invalid, 0, rgba, offset);
    } else {
      writeProgramColor(program, normalizeMetric(value, field.bailout), rgba, offset);
    }
  }
  return rgba;
}
