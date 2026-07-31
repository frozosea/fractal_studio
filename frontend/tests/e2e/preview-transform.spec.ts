import { expect, test } from "@playwright/test";
import {
  applyPreview,
  IDENTITY_PREVIEW,
  panBy,
  zoomAbout,
  type PreviewTransform,
} from "../../src/lib/utils/preview-transform";

/**
 * Pure math, so no browser fixture is requested and this runs anywhere.
 *
 * These guard the bug where the canvas stored a translation and a scale as two
 * independent numbers: the pan survived a zoom unscaled, so the image drifted
 * by `(1 - k) * d` and compounded with every further gesture.
 */

const CLOSE = 1e-9;

function expectPoint(actual: { x: number; y: number }, x: number, y: number) {
  expect(actual.x).toBeCloseTo(x, 9);
  expect(actual.y).toBeCloseTo(y, 9);
}

test("zoom keeps the point under the cursor fixed", () => {
  const zoomed = zoomAbout(IDENTITY_PREVIEW, 2.5, 120, 80);
  expectPoint(applyPreview(zoomed, 120, 80), 120, 80);
});

test("panning then zooming scales the pan, because the zoom acts on the panned image", () => {
  const k = 3;
  const [dx, dy] = [40, -25];
  const [cx, cy] = [200, 150];

  const composed = zoomAbout(panBy(IDENTITY_PREVIEW, dx, dy), k, cx, cy);

  // A source point p sits at p + d after the pan; zooming about c must then
  // send it to k*(p + d) + (1 - k)*c.
  const p = { x: 60, y: 90 };
  expectPoint(
    applyPreview(composed, p.x, p.y),
    k * (p.x + dx) + (1 - k) * cx,
    k * (p.y + dy) + (1 - k) * cy,
  );

  // The regression: leaving the pan unscaled lands (1 - k) * d away.
  const unscaled = { x: k * p.x + (1 - k) * cx + dx, y: k * p.y + (1 - k) * cy + dy };
  expect(Math.abs(applyPreview(composed, p.x, p.y).x - unscaled.x)).toBeCloseTo(Math.abs((1 - k) * dx), 9);
});

test("zooming then panning adds the drag unscaled, because a drag is screen space", () => {
  const zoomed = zoomAbout(IDENTITY_PREVIEW, 0.4, 200, 150);
  const [dx, dy] = [40, -25];
  const composed = panBy(zoomed, dx, dy);

  const before = applyPreview(zoomed, 60, 90);
  expectPoint(applyPreview(composed, 60, 90), before.x + dx, before.y + dy);
});

test("repeated zooms about different anchors compose like real affine maps", () => {
  // The exact case that broke: an absolute factor cannot express this, because
  // the second anchor acts on the result of the first zoom.
  const first = zoomAbout(IDENTITY_PREVIEW, 2, 100, 100);
  const composed = zoomAbout(first, 1.5, 300, 50);

  const p = { x: 40, y: 260 };
  const afterFirst = applyPreview(first, p.x, p.y);
  expectPoint(
    applyPreview(composed, p.x, p.y),
    1.5 * afterFirst.x + (1 - 1.5) * 300,
    1.5 * afterFirst.y + (1 - 1.5) * 50,
  );
  expect(composed.scale).toBeCloseTo(3, 9);
});

test("a long interleaved gesture stays exact rather than accumulating error", () => {
  // "It goes wrong as soon as you do more operations" — so drive many.
  let composed: PreviewTransform = IDENTITY_PREVIEW;
  const reference = (x: number, y: number) => ({ x, y });
  let point = reference(37, 211);

  for (let step = 0; step < 40; step += 1) {
    if (step % 3 === 0) {
      const k = step % 2 === 0 ? 1.17 : 0.83;
      const [cx, cy] = [50 + step * 7, 300 - step * 5];
      composed = zoomAbout(composed, k, cx, cy);
      point = { x: k * point.x + (1 - k) * cx, y: k * point.y + (1 - k) * cy };
    } else {
      const [dx, dy] = [step % 2 === 0 ? 13 : -9, step % 4 === 0 ? -6 : 11];
      composed = panBy(composed, dx, dy);
      point = { x: point.x + dx, y: point.y + dy };
    }
  }

  const actual = applyPreview(composed, 37, 211);
  expect(Math.hypot(actual.x - point.x, actual.y - point.y)).toBeLessThan(CLOSE);
});

test("identity draws the image untouched", () => {
  expectPoint(applyPreview(IDENTITY_PREVIEW, 123, 456), 123, 456);
  expect(IDENTITY_PREVIEW.scale).toBe(1);
});
