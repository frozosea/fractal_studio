"use client";

import { useEffect, useRef, useState } from "react";
import { Minus, Plus, LoaderCircle, RotateCcw } from "lucide-react";
import { Button } from "@/components/ui/button";
import type { FractalSpec } from "@/lib/api/platform";

type Props = {
  spec: FractalSpec;
  preview: string | null;
  previewing: boolean;
  width: number;
  height: number;
  onChange: (patch: Partial<FractalSpec>) => void;
  onReset: () => void;
  onZoom: (factor: number) => void;
  onNavigationStart: () => void;
  onPointSelect?: (point: { re: number; im: number }) => void;
  selection?: { re: number; im: number };
  exportWidth: number;
  exportHeight: number;
  labels: {
    alt: string;
    empty: string;
    hint: string;
    detail: string;
    rendering: string;
    zoomOut: string;
    zoomIn: string;
    reset: string;
    frame: string;
    panPreview: string;
    zoomPreview: string;
  };
};

type ViewportBox = { left: number; top: number; width: number; height: number };
type PreviewTransform = {
  kind: "pan" | "zoom";
  x: number;
  y: number;
  scale: number;
  originX: number;
  originY: number;
};

function worldFromClient(spec: FractalSpec, box: ViewportBox, clientX: number, clientY: number, scale = Number(spec.scale ?? 3)) {
  const aspect = box.width / box.height;
  const localRe = ((clientX - box.left) / box.width - 0.5) * scale * aspect;
  const localIm = (0.5 - (clientY - box.top) / box.height) * scale;
  const angle = Number(spec.rotationDeg ?? 0) * Math.PI / 180;
  const cos = Math.cos(angle);
  const sin = Math.sin(angle);
  return {
    re: Number(spec.centerRe ?? 0) + localRe * cos - localIm * sin,
    im: Number(spec.centerIm ?? 0) + localRe * sin + localIm * cos,
  };
}

export function InteractiveFractalCanvas({ spec, preview, previewing, width, height, onChange, onReset, onZoom, onNavigationStart, onPointSelect, selection, exportWidth, exportHeight, labels }: Props) {
  const element = useRef<HTMLDivElement>(null);
  const drag = useRef<{ x: number; y: number; re: number; im: number; moved: boolean } | null>(null);
  const wheelActive = useRef(false);
  const wheelTimer = useRef<number | null>(null);
  const wheelHandler = useRef<(event: WheelEvent) => void>(() => undefined);
  const zoomBaseScale = useRef<number | null>(null);
  const [previewTransform, setPreviewTransform] = useState<PreviewTransform | null>(null);

  useEffect(() => {
    setPreviewTransform(null);
    zoomBaseScale.current = null;
  }, [preview]);

  const move = (x: number, y: number, baseRe: number, baseIm: number, startX: number, startY: number) => {
    const box = element.current?.getBoundingClientRect();
    if (!box) return;
    const scale = Number(spec.scale ?? 3);
    const aspect = box.width / box.height;
    const localRe = ((x - startX) / box.width) * scale * aspect;
    const localIm = (-(y - startY) / box.height) * scale;
    const angle = Number(spec.rotationDeg ?? 0) * Math.PI / 180;
    const cos = Math.cos(angle);
    const sin = Math.sin(angle);
    onChange({ centerRe: baseRe - (localRe * cos - localIm * sin), centerIm: baseIm - (localRe * sin + localIm * cos) });
  };

  const marker = (() => {
    const box = element.current?.getBoundingClientRect();
    if (!selection || !box) return null;
    const scale = Number(spec.scale ?? 3);
    const angle = Number(spec.rotationDeg ?? 0) * Math.PI / 180;
    const cos = Math.cos(angle);
    const sin = Math.sin(angle);
    const deltaRe = selection.re - Number(spec.centerRe ?? 0);
    const deltaIm = selection.im - Number(spec.centerIm ?? 0);
    const localRe = deltaRe * cos + deltaIm * sin;
    const localIm = -deltaRe * sin + deltaIm * cos;
    return {
      left: 50 + (localRe / (scale * (box.width / box.height))) * 100,
      top: 50 - (localIm / scale) * 100,
    };
  })();

  wheelHandler.current = (event) => {
    event.preventDefault();
    event.stopPropagation();
    if (!wheelActive.current) { wheelActive.current = true; onNavigationStart(); }
    if (wheelTimer.current) window.clearTimeout(wheelTimer.current);
    wheelTimer.current = window.setTimeout(() => { wheelActive.current = false; }, 180);
    const box = element.current?.getBoundingClientRect();
    if (!box) return;
    const oldScale = Number(spec.scale ?? 3);
    const nextScale = Math.min(1e9, Math.max(1e-12, oldScale * Math.exp(event.deltaY * 0.0015)));
    const world = worldFromClient(spec, box, event.clientX, event.clientY);
    const nextAtCursor = worldFromClient({ ...spec, centerRe: 0, centerIm: 0 }, box, event.clientX, event.clientY, nextScale);
    const baseScale = zoomBaseScale.current ?? oldScale;
    zoomBaseScale.current = baseScale;
    setPreviewTransform({
      kind: "zoom",
      x: 0,
      y: 0,
      scale: baseScale / nextScale,
      originX: event.clientX - box.left,
      originY: event.clientY - box.top,
    });
    onChange({ scale: nextScale, centerRe: world.re - nextAtCursor.re, centerIm: world.im - nextAtCursor.im });
  };

  useEffect(() => {
    const canvas = element.current;
    if (!canvas) return;
    const handleWheel = (event: WheelEvent) => wheelHandler.current(event);
    canvas.addEventListener("wheel", handleWheel, { passive: false });
    return () => canvas.removeEventListener("wheel", handleWheel);
  }, []);

  useEffect(() => () => {
    if (wheelTimer.current) window.clearTimeout(wheelTimer.current);
  }, []);

  const zoomCentered = (factor: number) => {
    const box = element.current?.getBoundingClientRect();
    onNavigationStart();
    setPreviewTransform({
      kind: "zoom",
      x: 0,
      y: 0,
      scale: 1 / factor,
      originX: (box?.width ?? width) / 2,
      originY: (box?.height ?? height) / 2,
    });
    onZoom(factor);
  };

  return <div className="scientific-canvas relative mx-auto max-w-full overflow-hidden bg-black" ref={element}
    onPointerDown={(event) => {
      if ((event.target as HTMLElement).closest("button")) return;
      onNavigationStart(); event.currentTarget.setPointerCapture(event.pointerId);
      zoomBaseScale.current = null;
      setPreviewTransform({ kind: "pan", x: 0, y: 0, scale: 1, originX: 0, originY: 0 });
      drag.current = { x: event.clientX, y: event.clientY, re: Number(spec.centerRe ?? 0), im: Number(spec.centerIm ?? 0), moved: false };
    }}
    onPointerMove={(event) => {
      const value = drag.current;
      if (!value) return;
      if (Math.hypot(event.clientX - value.x, event.clientY - value.y) > 4) value.moved = true;
      if (value.moved) {
        setPreviewTransform({
          kind: "pan",
          x: event.clientX - value.x,
          y: event.clientY - value.y,
          scale: 1,
          originX: 0,
          originY: 0,
        });
        move(event.clientX, event.clientY, value.re, value.im, value.x, value.y);
      }
    }}
    onPointerUp={(event) => {
      const value = drag.current;
      drag.current = null;
      const box = element.current?.getBoundingClientRect();
      if (value && !value.moved) {
        setPreviewTransform(null);
        if (box && onPointSelect) onPointSelect(worldFromClient(spec, box, event.clientX, event.clientY));
      }
    }}
    onPointerCancel={() => { drag.current = null; }}
    onDoubleClick={(event) => {
      const box = element.current?.getBoundingClientRect(); if (!box) return;
      onNavigationStart();
      const oldScale = Number(spec.scale ?? 3); const nextScale = Math.max(1e-12, oldScale * 0.35);
      const world = worldFromClient(spec, box, event.clientX, event.clientY);
      const nextAtCursor = worldFromClient({ ...spec, centerRe: 0, centerIm: 0 }, box, event.clientX, event.clientY, nextScale);
      setPreviewTransform({ kind: "zoom", x: 0, y: 0, scale: oldScale / nextScale, originX: event.clientX - box.left, originY: event.clientY - box.top });
      onChange({ scale: nextScale, centerRe: world.re - nextAtCursor.re, centerIm: world.im - nextAtCursor.im });
    }}
    style={{
      touchAction: "none",
      overscrollBehavior: "contain",
      aspectRatio: `${width}/${height}`,
      height: "min(64dvh, 52rem)",
      width: "auto",
    }}>
    {preview ? <img
      src={preview}
      alt={labels.alt}
      draggable={false}
      className="h-full w-full select-none object-contain will-change-transform"
      style={previewTransform ? {
        transform: `translate3d(${previewTransform.x}px, ${previewTransform.y}px, 0) scale(${previewTransform.scale})`,
        transformOrigin: `${previewTransform.originX}px ${previewTransform.originY}px`,
        transition: previewTransform.kind === "zoom" ? "transform 100ms ease-out" : "none",
      } : undefined}
    /> : <div className="grid h-full min-h-[26rem] place-items-center text-sm text-white/45">{labels.empty}</div>}
    <div className="pointer-events-none absolute inset-0" aria-hidden="true">
      <span className="absolute left-1/2 top-0 h-full w-px bg-amber-300/10" />
      <span className="absolute left-0 top-1/2 h-px w-full bg-amber-300/10" />
      <span className="absolute left-1/2 top-1/2 h-2 w-2 -translate-x-1/2 -translate-y-1/2 border border-amber-300/45" />
      {marker && marker.left >= 0 && marker.left <= 100 && marker.top >= 0 && marker.top <= 100 && <span className="absolute h-3 w-3 -translate-x-1/2 -translate-y-1/2 rounded-full border-2 border-amber-300 bg-black/50 shadow-[0_0_0_2px_rgb(0_0_0/0.65)]" style={{ left: `${marker.left}%`, top: `${marker.top}%` }} />}
    </div>
    <div className="pointer-events-none absolute inset-x-0 bottom-0 flex items-center justify-between bg-black/75 px-3 py-2 font-mono text-[11px] text-white/65">
      <span>{labels.hint}</span><span>{spec.iterations} {labels.detail}</span>
    </div>
    <span className="pointer-events-none absolute left-3 top-3 border border-amber-300/30 bg-black/75 px-2 py-1 font-mono text-[10px] uppercase tracking-wider text-amber-200/80">{labels.frame} {exportWidth}×{exportHeight}</span>
    {previewTransform && <span className="pointer-events-none absolute left-3 top-11 border border-cyan-300/30 bg-black/75 px-2 py-1 font-mono text-[10px] uppercase tracking-wider text-cyan-100/80">
      {previewTransform.kind === "pan" ? labels.panPreview : labels.zoomPreview}
    </span>}
    {previewing && <div className="pointer-events-none absolute inset-0 grid place-items-center bg-black/25"><span className="flex items-center gap-2 border border-white/15 bg-black/80 px-3 py-1.5 text-sm"><LoaderCircle className="h-4 w-4 animate-spin" /> {labels.rendering}</span></div>}
    <div className="absolute right-3 top-3 flex overflow-hidden border border-white/20 bg-black/80">
      <Button aria-label={labels.zoomOut} title={labels.zoomOut} size="sm" variant="ghost" className="rounded-none" onClick={() => zoomCentered(2)}><Minus className="h-4 w-4" /></Button>
      <Button aria-label={labels.zoomIn} title={labels.zoomIn} size="sm" variant="ghost" className="rounded-none border-x border-white/15" onClick={() => zoomCentered(0.35)}><Plus className="h-4 w-4" /></Button>
      <Button aria-label={labels.reset} title={labels.reset} size="sm" variant="ghost" className="rounded-none" onClick={() => { setPreviewTransform(null); onReset(); }}><RotateCcw className="h-4 w-4" /></Button>
    </div>
  </div>;
}
