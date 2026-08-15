import type { LocalVariant } from "./local-render-core";

export const LOCAL_AXIS_TRANSITION_VARIANTS = [
  "mandelbrot",
  "tricorn",
  "burning_ship",
  "celtic",
  "heart",
  "buffalo",
  "perp_buffalo",
  "celtic_ship",
  "mandelceltic",
  "perp_ship",
] as const;

export type LocalAxisTransitionVariant = (typeof LOCAL_AXIS_TRANSITION_VARIANTS)[number];
export type LocalTransitionMetric = "escape" | "min_abs" | "max_abs" | "envelope" | "min_pairwise_dist";

export type LocalTransitionLeg = {
  variant: LocalAxisTransitionVariant;
  weight: number;
};

export type ActiveLocalTransitionLeg = {
  variant: LocalAxisTransitionVariant;
  /** Unit-length slice direction. */
  direction: number;
  /** Weight relative to the largest active leg. */
  influence: number;
};

/**
 * Browser fp64 equivalent of TransitionParams + its MapParams base.
 * `scale` is the logical viewport height. A missing/non-positive
 * `viewportAspect` uses width / height, exactly like map_viewport_aspect().
 */
export type LocalTransitionRenderSpec = {
  centerRe: number;
  centerIm: number;
  scale: number;
  viewportAspect?: number;
  iterations: number;
  metric: LocalTransitionMetric;
  bailout: number;
  bailoutSq?: number;
  rotationDeg: number;
  julia: boolean;
  juliaRe: number;
  juliaIm: number;
  transitionThetaMilliDeg: number;
  transitionFrom: LocalAxisTransitionVariant;
  transitionTo: LocalAxisTransitionVariant;
  pairwiseCap?: number;
  /** A non-empty list selects the N-way kernel; an empty list selects pair. */
  transitionLegs?: readonly LocalTransitionLeg[];
};

export type LocalTransitionOrbitSample = {
  /** Zero-based escape iteration, or max iterations when bounded. */
  iter: number;
  /** Squared orbit norm at escape, or zero when bounded. */
  norm: number;
  /** Raw value for non-escape metrics; zero for escape. */
  field: number;
  escaped: boolean;
};

export type LocalFp64RawField = {
  width: number;
  height: number;
  metric: LocalTransitionMetric | "mandel_ship_agree";
  /** Present only for escape. Mirrors FieldOutput::iter_u32. */
  iterU32?: Uint32Array;
  /** Present only for escape. Mirrors FieldOutput::norm_f32. */
  normF32?: Float32Array;
  /** Present for scalar fields. Mirrors FieldOutput::field_f64. */
  fieldF64?: Float64Array;
  /** Agreement renderer escape counts used by its inverted presentation. */
  agreementIterU32?: Uint32Array;
  fieldMin?: number;
  fieldMax?: number;
};

export type LocalRawRenderOptions = {
  shouldCancel?: () => boolean;
  onRowDone?: (row: number) => void;
};

export type LocalDirectTransitionSlice = "none" | "from" | "from_flip_y" | "to" | "to_flip_y";

export type LocalNormalizedTransitionTheta = {
  milliDeg: number;
  radians: number;
  directSlice: LocalDirectTransitionSlice;
};

export type LocalMandelShipAgreementSpec = {
  centerRe: number;
  centerIm: number;
  scale: number;
  viewportAspect?: number;
  iterations: number;
  variant: LocalVariant;
  bailout: number;
  bailoutSq?: number;
  julia: boolean;
  juliaRe: number;
  juliaIm: number;
  /** Kept for adapter compatibility. The backend agreement renderer ignores it. */
  rotationDeg?: number;
};

export type LocalMandelShipAgreementSample = {
  iter: number;
  fullyAgrees: boolean;
};

const PI = 3.14159265358979323846264338327950288;
const THETA_SCALE = 1000;
const THETA_HALF_TURN_MDEG = 180 * THETA_SCALE;
const THETA_FULL_TURN_MDEG = 360 * THETA_SCALE;
const MAX_TRANSITION_LEGS = 4;

const AXIS_VARIANT_SET = new Set<string>(LOCAL_AXIS_TRANSITION_VARIANTS);
const LOCAL_VARIANT_SET = new Set<string>([
  ...LOCAL_AXIS_TRANSITION_VARIANTS,
  "sin_z",
  "cos_z",
  "exp_z",
  "sinh_z",
  "cosh_z",
  "tan_z",
]);

type ViewportSpec = {
  centerRe: number;
  centerIm: number;
  scale: number;
  viewportAspect?: number;
  rotationDeg: number;
};

type PairPlan = {
  kind: "pair";
  theta: LocalNormalizedTransitionTheta;
};

type MultiPlan = {
  kind: "multi";
  legs: readonly ActiveLocalTransitionLeg[];
};

type TransitionPlan = PairPlan | MultiPlan;

/** Equivalent to normalize_transition_milli_deg(). */
export function normalizeLocalTransitionMilliDeg(milliDeg: number): number {
  if (!Number.isSafeInteger(milliDeg)) {
    throw new RangeError("transitionThetaMilliDeg must be a safe integer");
  }
  let wrapped = (milliDeg + THETA_HALF_TURN_MDEG) % THETA_FULL_TURN_MDEG;
  if (wrapped < 0) wrapped += THETA_FULL_TURN_MDEG;
  wrapped -= THETA_HALF_TURN_MDEG;
  if (wrapped === -THETA_HALF_TURN_MDEG && milliDeg > 0) {
    wrapped = THETA_HALF_TURN_MDEG;
  }
  return wrapped;
}

export function resolveLocalTransitionTheta(milliDeg: number): LocalNormalizedTransitionTheta {
  const normalized = normalizeLocalTransitionMilliDeg(milliDeg);
  let directSlice: LocalDirectTransitionSlice = "none";
  if (normalized === 0) directSlice = "from";
  else if (normalized === 90 * THETA_SCALE) directSlice = "to";
  else if (normalized === -90 * THETA_SCALE) directSlice = "to_flip_y";
  else if (normalized === THETA_HALF_TURN_MDEG || normalized === -THETA_HALF_TURN_MDEG) {
    directSlice = "from_flip_y";
  }
  return {
    milliDeg: normalized,
    radians: normalized * PI / (180 * THETA_SCALE),
    directSlice,
  };
}

/** Equivalent to active_transition_legs(). */
export function activateLocalTransitionLegs(
  input: readonly LocalTransitionLeg[],
): readonly ActiveLocalTransitionLeg[] {
  if (input.length > MAX_TRANSITION_LEGS) {
    throw new RangeError("multi transition supports at most 4 variants");
  }

  let maximumWeight = 0;
  let squaredWeightSum = 0;
  const kept: LocalTransitionLeg[] = [];
  for (const leg of input) {
    assertAxisVariant(leg.variant);
    if (!Number.isFinite(leg.weight)) throw new RangeError("invalid transition weight");
    if (leg.weight <= 0) continue;
    kept.push(leg);
    if (leg.weight > maximumWeight) maximumWeight = leg.weight;
    squaredWeightSum += leg.weight * leg.weight;
  }
  if (kept.length === 0 || maximumWeight <= 0 || squaredWeightSum <= 0) {
    throw new RangeError("multi transition needs at least one positive-weight variant");
  }

  const inverseLength = 1 / Math.sqrt(squaredWeightSum);
  return kept.map((leg) => ({
    variant: leg.variant,
    direction: leg.weight * inverseLength,
    influence: leg.weight / maximumWeight,
  }));
}

/**
 * Iterate one logical viewport point. `u` and `v` are coordinates after view
 * rotation. Exact cardinal pair angles take the backend's direct 2D path.
 */
export function iterateLocalTransitionPoint(
  spec: LocalTransitionRenderSpec,
  u: number,
  v: number,
): LocalTransitionOrbitSample {
  validateTransitionSpec(spec);
  const plan = makeTransitionPlan(spec);
  return iterateTransitionPointWithPlan(spec, plan, u, v);
}

/** Render raw fp64 transition data suitable for Worker-side colorization. */
export function renderLocalTransitionRaw(
  spec: LocalTransitionRenderSpec,
  width: number,
  height: number,
  options: LocalRawRenderOptions = {},
): LocalFp64RawField {
  validateDimensions(width, height);
  validateTransitionSpec(spec);
  const plan = makeTransitionPlan(spec);
  const pixelCount = width * height;
  const isEscape = spec.metric === "escape";
  const iterU32 = isEscape ? new Uint32Array(pixelCount) : undefined;
  const normF32 = isEscape ? new Float32Array(pixelCount) : undefined;
  const fieldF64 = isEscape ? undefined : new Float64Array(pixelCount);
  let fieldMin = Number.POSITIVE_INFINITY;
  let fieldMax = Number.NEGATIVE_INFINITY;

  const direct = plan.kind === "pair" ? plan.theta.directSlice : "none";
  const directFlip = direct === "from_flip_y" || direct === "to_flip_y";
  const directVariant = direct === "to" || direct === "to_flip_y"
    ? spec.transitionTo
    : spec.transitionFrom;
  const directViewport: ViewportSpec = directFlip
    ? {
        centerRe: spec.centerRe,
        centerIm: -spec.centerIm,
        scale: spec.scale,
        viewportAspect: spec.viewportAspect,
        rotationDeg: -spec.rotationDeg,
      }
    : spec;

  for (let y = 0; y < height; y += 1) {
    if (options.shouldCancel?.()) throw new Error("cancelled");
    const directSourceY = directFlip ? height - 1 - y : y;
    for (let x = 0; x < width; x += 1) {
      let sample: LocalTransitionOrbitSample;
      if (direct !== "none") {
        const [u, v] = directMapViewportPoint(directViewport, width, height, x, directSourceY);
        sample = iterateDirectMap(
          spec,
          directVariant,
          u,
          v,
          directFlip ? -spec.juliaIm : spec.juliaIm,
        );
      } else {
        const [u, v] = transitionViewportPoint(spec, width, height, x, y);
        sample = iterateTransitionPointWithPlan(spec, plan, u, v);
      }

      const index = y * width + x;
      if (isEscape) {
        iterU32![index] = sample.escaped ? sample.iter : spec.iterations;
        normF32![index] = sample.escaped ? sample.norm : 0;
      } else {
        fieldF64![index] = sample.field;
        if (sample.field < fieldMin) fieldMin = sample.field;
        if (sample.field > fieldMax) fieldMax = sample.field;
      }
    }
    options.onRowDone?.(y);
  }

  if (isEscape) {
    return { width, height, metric: "escape", iterU32, normF32 };
  }

  // Exact cardinal slices delegate to render_map_field(), whose fp64 scalar
  // path filters non-finite extrema. Non-cardinal pair/multi code does not.
  if (direct !== "none") {
    fieldMin = Number.POSITIVE_INFINITY;
    fieldMax = Number.NEGATIVE_INFINITY;
    for (const value of fieldF64!) {
      if (!Number.isFinite(value)) continue;
      if (value < fieldMin) fieldMin = value;
      if (value > fieldMax) fieldMax = value;
    }
    if (!Number.isFinite(fieldMin)) fieldMin = 0;
    if (!Number.isFinite(fieldMax)) fieldMax = 1;
  }
  return { width, height, metric: spec.metric, fieldF64, fieldMin, fieldMax };
}

/**
 * Backend-compatible Mandelbrot/variant agreement for one map point. This is
 * not two independent orbits: the Mandelbrot step is compared at each point
 * of the selected variant's orbit, as variant_explore_orbit() does.
 */
export function iterateLocalMandelShipAgreementPoint(
  spec: LocalMandelShipAgreementSpec,
  re: number,
  im: number,
): LocalMandelShipAgreementSample {
  validateAgreementSpec(spec);
  return iterateLocalMandelShipAgreementPointUnchecked(spec, re, im);
}

/** Render the backend's 0/1 mandel_ship_agree raw field. */
export function renderLocalMandelShipAgreementRaw(
  spec: LocalMandelShipAgreementSpec,
  width: number,
  height: number,
  options: LocalRawRenderOptions = {},
): LocalFp64RawField {
  validateDimensions(width, height);
  validateAgreementSpec(spec);
  const fieldF64 = new Float64Array(width * height);
  const agreementIterU32 = new Uint32Array(width * height);
  const aspect = effectiveAspect(spec.viewportAspect, width, height);
  const spanIm = spec.scale;
  const spanRe = spec.scale * aspect;
  const reMin = spec.centerRe - spanRe * 0.5;
  const imMax = spec.centerIm + spanIm * 0.5;

  // explore_pixels() intentionally ignores MapParams::rotation_deg. Preserve
  // that observable behavior so browser/server raw fields remain comparable.
  for (let y = 0; y < height; y += 1) {
    if (options.shouldCancel?.()) throw new Error("cancelled");
    const im = imMax - (y + 0.5) / height * spanIm;
    for (let x = 0; x < width; x += 1) {
      const re = reMin + (x + 0.5) / width * spanRe;
      const sample = iterateLocalMandelShipAgreementPointUnchecked(spec, re, im);
      const index = y * width + x;
      fieldF64[index] = sample.fullyAgrees ? 1 : 0;
      agreementIterU32[index] = sample.iter;
    }
    options.onRowDone?.(y);
  }

  return {
    width,
    height,
    metric: "mandel_ship_agree",
    fieldF64,
    agreementIterU32,
    fieldMin: 0,
    fieldMax: 1,
  };
}

function makeTransitionPlan(spec: LocalTransitionRenderSpec): TransitionPlan {
  const legs = spec.transitionLegs;
  if (legs && legs.length > 0) {
    return { kind: "multi", legs: activateLocalTransitionLegs(legs) };
  }
  return { kind: "pair", theta: resolveLocalTransitionTheta(spec.transitionThetaMilliDeg) };
}

function iterateTransitionPointWithPlan(
  spec: LocalTransitionRenderSpec,
  plan: TransitionPlan,
  u: number,
  v: number,
): LocalTransitionOrbitSample {
  if (plan.kind === "multi") return iterateMulti(spec, plan.legs, u, v);

  switch (plan.theta.directSlice) {
    case "from":
      return iterateDirectMap(spec, spec.transitionFrom, u, v, spec.juliaIm);
    case "to":
      return iterateDirectMap(spec, spec.transitionTo, u, v, spec.juliaIm);
    case "from_flip_y":
      return iterateDirectMap(spec, spec.transitionFrom, u, -v, -spec.juliaIm);
    case "to_flip_y":
      return iterateDirectMap(spec, spec.transitionTo, u, -v, -spec.juliaIm);
    case "none":
      return iteratePair(spec, plan.theta.radians, u, v);
  }
}

function iteratePair(
  spec: LocalTransitionRenderSpec,
  theta: number,
  u: number,
  v: number,
): LocalTransitionOrbitSample {
  const cosine = Math.cos(theta);
  const sine = Math.sin(theta);
  const x0 = u;
  const y0 = v * cosine;
  const z0 = v * sine;
  const cx = spec.julia ? spec.juliaRe : x0;
  const cy = spec.julia ? spec.juliaIm * cosine : y0;
  const cz = spec.julia ? spec.juliaIm * sine : z0;
  const bailoutSq = effectiveBailoutSq(spec);

  let x = x0;
  let y = y0;
  let z = z0;
  let x2 = x * x;
  let y2 = y * y;
  let z2 = z * z;
  let minimumSq = x2 + y2 + z2;
  let maximumSq = minimumSq;
  const orbit: Array<readonly [number, number, number]> = spec.metric === "min_pairwise_dist" ? [[x, y, z]] : [];

  for (let iteration = 0; iteration < spec.iterations; iteration += 1) {
    const nextX = transitionRealProjection(spec.transitionFrom, x2, y2)
      + transitionRealProjection(spec.transitionTo, x2, z2)
      - x2 + cx;
    const nextY = transitionImagProjection(spec.transitionFrom, x, y) + cy;
    const nextZ = transitionImagProjection(spec.transitionTo, x, z) + cz;
    const finite = Number.isFinite(nextX) && Number.isFinite(nextY) && Number.isFinite(nextZ);
    const nextX2 = finite ? nextX * nextX : Number.POSITIVE_INFINITY;
    const nextY2 = finite ? nextY * nextY : Number.POSITIVE_INFINITY;
    const nextZ2 = finite ? nextZ * nextZ : Number.POSITIVE_INFINITY;
    const normSq = finite ? nextX2 + nextY2 + nextZ2 : Number.POSITIVE_INFINITY;
    if (normSq < minimumSq) minimumSq = normSq;
    if (normSq > maximumSq) maximumSq = normSq;
    if (spec.metric === "min_pairwise_dist" && orbit.length < effectiveTransitionPairwiseCap(spec)) {
      orbit.push([nextX, nextY, nextZ]);
    }
    if (!finite || normSq > bailoutSq) {
      return orbitSample(spec.metric, iteration, normSq, true, minimumSq, maximumSq, false, minimumPairwise(orbit));
    }
    x = nextX;
    y = nextY;
    z = nextZ;
    x2 = nextX2;
    y2 = nextY2;
    z2 = nextZ2;
  }

  return orbitSample(spec.metric, spec.iterations, 0, false, minimumSq, maximumSq, false, minimumPairwise(orbit));
}

function iterateMulti(
  spec: LocalTransitionRenderSpec,
  legs: readonly ActiveLocalTransitionLeg[],
  u: number,
  v: number,
): LocalTransitionOrbitSample {
  const bailoutSq = effectiveBailoutSq(spec);
  const cx = spec.julia ? spec.juliaRe : u;
  let x = u;
  let x2 = x * x;
  const axis = legs.map((leg) => v * leg.direction);
  const axis2 = axis.map((value) => value * value);
  const constants = legs.map((leg, index) => spec.julia
    ? spec.juliaIm * leg.direction
    : axis[index]!);
  let initialNormSq = x2;
  for (const value of axis2) initialNormSq += value;
  let minimumSq = initialNormSq;
  let maximumSq = initialNormSq;
  const orbit: number[][] = spec.metric === "min_pairwise_dist" ? [[x, ...axis]] : [];
  const nextAxis = new Array<number>(legs.length).fill(0);

  for (let iteration = 0; iteration < spec.iterations; iteration += 1) {
    let realSum = 0;
    let influenceSum = 0;
    for (let index = 0; index < legs.length; index += 1) {
      const leg = legs[index]!;
      realSum += leg.influence * transitionRealProjection(leg.variant, x2, axis2[index]!);
      influenceSum += leg.influence;
      nextAxis[index] = leg.influence * transitionImagProjection(leg.variant, x, axis[index]!)
        + constants[index]!;
    }

    const nextX = realSum - (influenceSum - 1) * x2 + cx;
    const finiteX = Number.isFinite(nextX);
    const nextX2 = finiteX ? nextX * nextX : Number.POSITIVE_INFINITY;
    let normSq = finiteX ? nextX2 : Number.POSITIVE_INFINITY;
    let finiteAll = finiteX;
    for (let index = 0; index < nextAxis.length; index += 1) {
      const value = nextAxis[index]!;
      if (!Number.isFinite(value)) {
        finiteAll = false;
        normSq = Number.POSITIVE_INFINITY;
        break;
      }
      normSq += value * value;
    }
    if (normSq < minimumSq) minimumSq = normSq;
    if (normSq > maximumSq) maximumSq = normSq;
    if (spec.metric === "min_pairwise_dist" && orbit.length < effectiveTransitionPairwiseCap(spec)) {
      orbit.push([nextX, ...nextAxis]);
    }
    if (!finiteAll || normSq > bailoutSq) {
      return orbitSample(spec.metric, iteration, normSq, true, minimumSq, maximumSq, false, minimumPairwise(orbit));
    }

    x = nextX;
    x2 = nextX2;
    for (let index = 0; index < axis.length; index += 1) {
      const value = nextAxis[index]!;
      axis[index] = value;
      axis2[index] = value * value;
    }
  }

  return orbitSample(spec.metric, spec.iterations, 0, false, minimumSq, maximumSq, false, minimumPairwise(orbit));
}

function iterateDirectMap(
  spec: LocalTransitionRenderSpec,
  variant: LocalAxisTransitionVariant,
  re: number,
  im: number,
  juliaIm: number,
): LocalTransitionOrbitSample {
  let x = spec.julia ? re : 0;
  let y = spec.julia ? im : 0;
  const cx = spec.julia ? spec.juliaRe : re;
  const cy = spec.julia ? juliaIm : im;
  const bailoutSq = effectiveBailoutSq(spec);
  let minimumSq = Number.POSITIVE_INFINITY;
  let maximumSq = 0;
  const orbit: Array<readonly [number, number]> = [];
  let minimumPairwiseSq = Number.POSITIVE_INFINITY;

  const iterationLimit = spec.metric === "min_pairwise_dist" ? effectiveMapPairwiseCap(spec) : spec.iterations;
  for (let iteration = 0; iteration < iterationLimit; iteration += 1) {
    const [nextX, nextY] = stepAxisVariant(variant, x, y, cx, cy);
    const finite = Number.isFinite(nextX) && Number.isFinite(nextY);
    const normSq = finite
      ? nextX * nextX + nextY * nextY
      : Number.POSITIVE_INFINITY;
    if (normSq < minimumSq) minimumSq = normSq;
    if (normSq > maximumSq) maximumSq = normSq;
    if (spec.metric === "min_pairwise_dist") {
      for (const [priorX, priorY] of orbit) {
        const dx = nextX - priorX; const dy = nextY - priorY;
        const distanceSq = dx * dx + dy * dy;
        if (distanceSq < minimumPairwiseSq) minimumPairwiseSq = distanceSq;
      }
      orbit.push([nextX, nextY]);
    }
    if (!finite || normSq > bailoutSq) {
      return orbitSample(spec.metric, iteration, normSq, true, minimumSq, maximumSq, true, Math.sqrt(minimumPairwiseSq));
    }
    x = nextX;
    y = nextY;
  }

  return orbitSample(spec.metric, spec.iterations, 0, false, minimumSq, maximumSq, true, Math.sqrt(minimumPairwiseSq));
}

function orbitSample(
  metric: LocalTransitionMetric,
  iter: number,
  norm: number,
  escaped: boolean,
  minimumSq: number,
  maximumSq: number,
  directMap: boolean,
  pairwise = 0,
): LocalTransitionOrbitSample {
  let field = 0;
  if (metric === "min_pairwise_dist") {
    field = Number.isFinite(pairwise) ? pairwise : 0;
  } else if (directMap) {
    const minimum = Number.isFinite(minimumSq) ? Math.sqrt(minimumSq) : Number.POSITIVE_INFINITY;
    const maximum = maximumSq !== 0 ? Math.sqrt(maximumSq) : 0;
    if (metric === "min_abs") field = Number.isFinite(minimum) ? minimum : 0;
    else if (metric === "max_abs") field = maximum > 0 ? maximum : 0;
    else if (metric === "envelope") field = Number.isFinite(minimum) ? 0.5 * (minimum + maximum) : 0;
  } else if (metric === "min_abs") {
    field = Math.sqrt(minimumSq);
  } else if (metric === "max_abs") {
    field = Math.sqrt(maximumSq);
  } else if (metric === "envelope") {
    field = 0.5 * (Math.sqrt(minimumSq) + Math.sqrt(maximumSq));
  }
  return { iter, norm, field, escaped };
}

function effectiveMapPairwiseCap(spec: LocalTransitionRenderSpec): number {
  return Math.max(1, Math.min(spec.iterations, Math.round(spec.pairwiseCap ?? 64)));
}

function effectiveTransitionPairwiseCap(spec: LocalTransitionRenderSpec): number {
  return Math.max(1, Math.round(spec.pairwiseCap ?? 64));
}

function minimumPairwise(points: ReadonlyArray<ReadonlyArray<number>>, current = Number.POSITIVE_INFINITY): number {
  if (points.length < 2) return Math.sqrt(current);
  for (let left = 0; left < points.length; left += 1) for (let right = left + 1; right < points.length; right += 1) {
    let distanceSq = 0;
    for (let axis = 0; axis < points[left]!.length; axis += 1) {
      const delta = points[left]![axis]! - points[right]![axis]!;
      distanceSq += delta * delta;
    }
    if (distanceSq < current) current = distanceSq;
  }
  return Math.sqrt(current);
}

function transitionRealProjection(
  variant: LocalAxisTransitionVariant,
  x2: number,
  axis2: number,
): number {
  const value = x2 - axis2;
  switch (variant) {
    case "buffalo":
    case "perp_buffalo":
    case "celtic_ship":
    case "mandelceltic":
    case "perp_ship":
      return Math.abs(value);
    default:
      return value;
  }
}

function transitionImagProjection(
  variant: LocalAxisTransitionVariant,
  x: number,
  axis: number,
): number {
  switch (variant) {
    case "tricorn":
    case "perp_buffalo":
      return -2 * x * axis;
    case "burning_ship":
    case "celtic_ship":
      return 2 * Math.abs(x * axis);
    case "celtic":
    case "mandelceltic":
      return 2 * x * Math.abs(axis);
    case "heart":
    case "perp_ship":
      return -2 * Math.abs(x) * axis;
    case "mandelbrot":
    case "buffalo":
      return 2 * x * axis;
  }
}

function stepAxisVariant(
  variant: LocalAxisTransitionVariant,
  x: number,
  y: number,
  cx: number,
  cy: number,
): readonly [number, number] {
  const x2 = x * x;
  const y2 = y * y;
  const squareReal = x2 - y2;
  switch (variant) {
    case "mandelbrot":
      return [squareReal + cx, 2 * x * y + cy];
    case "tricorn":
      return [squareReal + cx, -(2 * x * y) + cy];
    case "burning_ship":
      return [squareReal + cx, 2 * Math.abs(x) * Math.abs(y) + cy];
    case "celtic":
      return [squareReal + cx, 2 * x * Math.abs(y) + cy];
    case "heart":
      return [squareReal + cx, 2 * Math.abs(x) * (-y) + cy];
    case "buffalo":
      return [Math.abs(squareReal) + cx, 2 * x * y + cy];
    case "perp_buffalo":
      return [Math.abs(squareReal) + cx, -(2 * x * y) + cy];
    case "celtic_ship": {
      const squareImaginary = 2 * x * y;
      return [Math.abs(squareReal) + cx, Math.abs(squareImaginary) + cy];
    }
    case "mandelceltic":
      return [Math.abs(squareReal) + cx, 2 * x * Math.abs(y) + cy];
    case "perp_ship": {
      const squareImaginary = 2 * Math.abs(x) * y;
      return [Math.abs(squareReal) + cx, -squareImaginary + cy];
    }
  }
}

function stepBuiltinVariant(
  variant: LocalVariant,
  x: number,
  y: number,
  cx: number,
  cy: number,
): readonly [number, number] {
  if (AXIS_VARIANT_SET.has(variant)) {
    return stepAxisVariant(variant as LocalAxisTransitionVariant, x, y, cx, cy);
  }
  switch (variant) {
    case "sin_z":
      return [Math.sin(x) * Math.cosh(y) + cx, Math.cos(x) * Math.sinh(y) + cy];
    case "cos_z":
      return [Math.cos(x) * Math.cosh(y) + cx, -Math.sin(x) * Math.sinh(y) + cy];
    case "exp_z": {
      const exponential = Math.exp(x);
      return [exponential * Math.cos(y) + cx, exponential * Math.sin(y) + cy];
    }
    case "sinh_z":
      return [Math.sinh(x) * Math.cos(y) + cx, Math.cosh(x) * Math.sin(y) + cy];
    case "cosh_z":
      return [Math.cosh(x) * Math.cos(y) + cx, Math.sinh(x) * Math.sin(y) + cy];
    case "tan_z": {
      const denominator = Math.cos(2 * x) + Math.cosh(2 * y);
      if (denominator === 0) return [cx, cy];
      return [Math.sin(2 * x) / denominator + cx, Math.sinh(2 * y) / denominator + cy];
    }
    default:
      // Runtime guards reject this path; retain exhaustiveness for callers
      // compiled against a wider string type.
      throw new RangeError(`unsupported local variant: ${String(variant)}`);
  }
}

function transitionViewportPoint(
  spec: ViewportSpec,
  width: number,
  height: number,
  x: number,
  y: number,
): readonly [number, number] {
  const aspect = effectiveAspect(spec.viewportAspect, width, height);
  const spanIm = spec.scale;
  const spanRe = spec.scale * aspect;
  if (spec.rotationDeg !== 0) {
    const radians = spec.rotationDeg * PI / 180;
    const cosine = Math.cos(radians);
    const sine = Math.sin(radians);
    const pixelStepX = spanRe / width;
    const pixelStepY = spanIm / height;
    const dx = (x + 0.5 - width * 0.5) * pixelStepX;
    const dy = -(y + 0.5 - height * 0.5) * pixelStepY;
    return [
      spec.centerRe + dx * cosine - dy * sine,
      spec.centerIm + dx * sine + dy * cosine,
    ];
  }
  const reMin = spec.centerRe - spanRe * 0.5;
  const imMax = spec.centerIm + spanIm * 0.5;
  return [
    reMin + (x + 0.5) / width * spanRe,
    imMax - (y + 0.5) / height * spanIm,
  ];
}

/** fp64 field_variant_impl() uses cached inverse dimensions in this path. */
function directMapViewportPoint(
  spec: ViewportSpec,
  width: number,
  height: number,
  x: number,
  y: number,
): readonly [number, number] {
  const aspect = effectiveAspect(spec.viewportAspect, width, height);
  const spanIm = spec.scale;
  const spanRe = spec.scale * aspect;
  if (spec.rotationDeg !== 0) return transitionViewportPoint(spec, width, height, x, y);
  const reMin = spec.centerRe - spanRe * 0.5;
  const imMax = spec.centerIm + spanIm * 0.5;
  const inverseWidth = 1 / width;
  const inverseHeight = 1 / height;
  return [
    reMin + (x + 0.5) * inverseWidth * spanRe,
    imMax - (y + 0.5) * inverseHeight * spanIm,
  ];
}

function iterateLocalMandelShipAgreementPointUnchecked(
  spec: LocalMandelShipAgreementSpec,
  re: number,
  im: number,
): LocalMandelShipAgreementSample {
  const bailoutSq = effectiveBailoutSq(spec);
  let x = spec.julia ? re : 0;
  let y = spec.julia ? im : 0;
  const cx = spec.julia ? spec.juliaRe : re;
  const cy = spec.julia ? spec.juliaIm : im;
  let diverged = false;

  for (let iteration = 0; iteration < spec.iterations; iteration += 1) {
    const [nextX, nextY] = stepBuiltinVariant(spec.variant, x, y, cx, cy);
    if (!diverged) {
      const [mandelX, mandelY] = stepBuiltinVariant("mandelbrot", x, y, cx, cy);
      const tolerance = 1e-11 * (1 + Math.abs(mandelX) + Math.abs(mandelY));
      if (Math.abs(nextX - mandelX) > tolerance || Math.abs(nextY - mandelY) > tolerance) {
        diverged = true;
      }
    }
    x = nextX;
    y = nextY;
    if (x * x + y * y > bailoutSq) return { iter: iteration, fullyAgrees: !diverged };
  }
  return { iter: spec.iterations, fullyAgrees: !diverged };
}

function effectiveAspect(viewportAspect: number | undefined, width: number, height: number): number {
  return viewportAspect !== undefined && Number.isFinite(viewportAspect) && viewportAspect > 0
    ? viewportAspect
    : width / height;
}

function effectiveBailoutSq(spec: { bailout: number; bailoutSq?: number }): number {
  return spec.bailoutSq ?? spec.bailout * spec.bailout;
}

function assertAxisVariant(variant: string): asserts variant is LocalAxisTransitionVariant {
  if (!AXIS_VARIANT_SET.has(variant)) {
    throw new RangeError("transition variants must be quadratic Mandelbrot-family variants");
  }
}

function validateTransitionSpec(spec: LocalTransitionRenderSpec): void {
  if (!spec.transitionLegs || spec.transitionLegs.length === 0) {
    assertAxisVariant(spec.transitionFrom);
    assertAxisVariant(spec.transitionTo);
  }
  validateCommonSpec(spec);
  if (!Number.isFinite(spec.rotationDeg)) throw new RangeError("invalid rotationDeg");
  if (!Number.isSafeInteger(spec.transitionThetaMilliDeg)) {
    throw new RangeError("transitionThetaMilliDeg must be a safe integer");
  }
}

function validateAgreementSpec(spec: LocalMandelShipAgreementSpec): void {
  if (!LOCAL_VARIANT_SET.has(spec.variant)) throw new RangeError("unsupported local variant");
  validateCommonSpec(spec);
}

function validateCommonSpec(spec: {
  centerRe: number;
  centerIm: number;
  scale: number;
  viewportAspect?: number;
  iterations: number;
  bailout: number;
  bailoutSq?: number;
  juliaRe: number;
  juliaIm: number;
}): void {
  if (!Number.isFinite(spec.centerRe) || !Number.isFinite(spec.centerIm)) {
    throw new RangeError("invalid viewport center");
  }
  if (!Number.isFinite(spec.scale) || spec.scale <= 0) throw new RangeError("invalid scale");
  if (!Number.isSafeInteger(spec.iterations) || spec.iterations < 1) {
    throw new RangeError("iterations must be a positive safe integer");
  }
  if (!Number.isFinite(spec.bailout) || spec.bailout <= 0) throw new RangeError("invalid bailout");
  const bailoutSq = effectiveBailoutSq(spec);
  if (!Number.isFinite(bailoutSq) || bailoutSq <= 0) throw new RangeError("invalid bailoutSq");
  if (!Number.isFinite(spec.juliaRe) || !Number.isFinite(spec.juliaIm)) {
    throw new RangeError("invalid Julia parameter");
  }
}

function validateDimensions(width: number, height: number): void {
  if (!Number.isSafeInteger(width) || width < 1 || !Number.isSafeInteger(height) || height < 1) {
    throw new RangeError("render dimensions must be positive safe integers");
  }
  if (width * height > 0xffff_ffff) throw new RangeError("render dimensions are too large");
}
