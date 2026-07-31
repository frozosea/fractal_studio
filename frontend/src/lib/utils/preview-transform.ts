/**
 * The optimistic transform shown on the studio canvas while the server renders
 * the next preview, stored as the affine map it actually is: a screen point `p`
 * is drawn at `scale * p + (tx, ty)`.
 *
 * Holding a translation and a scale as two independent numbers cannot work,
 * because the correct result depends on the order the gestures happened in.
 * Panning by `d` and then zooming by `k` about the cursor has to scale that pan
 * — the zoom acts on the already-panned image — while zooming and then panning
 * must add `d` unscaled, since a drag is a screen-space displacement. Composing
 * the maps gets both for free; treating them as independent drifts by
 * `(1 - k) * d` and compounds with every further gesture.
 *
 * `panning`/`zooming` are presentation only: which label the badge shows, and
 * whether easing helps or lags.
 */
export type PreviewTransform = {
  panning: boolean;
  zooming: boolean;
  scale: number;
  tx: number;
  ty: number;
};

export const IDENTITY_PREVIEW: PreviewTransform = {
  panning: false,
  zooming: false,
  scale: 1,
  tx: 0,
  ty: 0,
};

/**
 * Compose a zoom of `k` centred on `(cx, cy)`, in coordinates relative to the
 * element's own origin. Every gesture contributes an *increment*: an absolute
 * factor would discard how earlier zooms about different anchors composed.
 */
export function zoomAbout(
  current: PreviewTransform,
  k: number,
  cx: number,
  cy: number,
): PreviewTransform {
  return {
    ...current,
    zooming: true,
    scale: current.scale * k,
    tx: k * current.tx + (1 - k) * cx,
    ty: k * current.ty + (1 - k) * cy,
  };
}

/** Compose a screen-space translation onto `current`. */
export function panBy(current: PreviewTransform, dx: number, dy: number): PreviewTransform {
  return { ...current, panning: true, tx: current.tx + dx, ty: current.ty + dy };
}

/** Where the transform puts a point that started at `(x, y)`. */
export function applyPreview(transform: PreviewTransform, x: number, y: number): { x: number; y: number } {
  return { x: transform.scale * x + transform.tx, y: transform.scale * y + transform.ty };
}

/** The CSS this maps to. `transform-origin` must stay at the element origin. */
export function previewTransformCss(transform: PreviewTransform): string {
  return `translate3d(${transform.tx}px, ${transform.ty}px, 0) scale(${transform.scale})`;
}
