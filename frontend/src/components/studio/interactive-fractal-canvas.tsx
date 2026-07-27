"use client";

import { useRef } from "react";
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
  };
};

export function InteractiveFractalCanvas({ spec, preview, previewing, width, height, onChange, onReset, onZoom, onNavigationStart, exportWidth, exportHeight, labels }: Props) {
  const element = useRef<HTMLDivElement>(null);
  const drag = useRef<{ x: number; y: number; re: number; im: number } | null>(null);
  const wheelActive = useRef(false);
  const wheelTimer = useRef<number | null>(null);

  const move = (x: number, y: number, baseRe: number, baseIm: number, startX: number, startY: number) => {
    const box = element.current?.getBoundingClientRect();
    if (!box) return;
    const scale = Number(spec.scale ?? 3);
    const aspect = box.width / box.height;
    onChange({ centerRe: baseRe - ((x - startX) / box.width) * scale * aspect, centerIm: baseIm + ((y - startY) / box.height) * scale });
  };

  return <div className="scientific-canvas relative overflow-hidden bg-black" ref={element}
    onPointerDown={(event) => {
      if ((event.target as HTMLElement).closest("button")) return;
      onNavigationStart(); event.currentTarget.setPointerCapture(event.pointerId);
      drag.current = { x: event.clientX, y: event.clientY, re: Number(spec.centerRe ?? 0), im: Number(spec.centerIm ?? 0) };
    }}
    onPointerMove={(event) => { const value = drag.current; if (value) move(event.clientX, event.clientY, value.re, value.im, value.x, value.y); }}
    onPointerUp={() => { drag.current = null; }}
    onPointerCancel={() => { drag.current = null; }}
    onWheel={(event) => {
      event.preventDefault();
      if (!wheelActive.current) { wheelActive.current = true; onNavigationStart(); }
      if (wheelTimer.current) window.clearTimeout(wheelTimer.current);
      wheelTimer.current = window.setTimeout(() => { wheelActive.current = false; }, 180);
      const box = element.current?.getBoundingClientRect(); if (!box) return;
      const oldScale = Number(spec.scale ?? 3); const nextScale = Math.min(1e9, Math.max(1e-12, oldScale * Math.exp(event.deltaY * 0.0015)));
      const x = (event.clientX - box.left) / box.width - 0.5; const y = (event.clientY - box.top) / box.height - 0.5; const aspect = box.width / box.height;
      const worldRe = Number(spec.centerRe ?? 0) + x * oldScale * aspect; const worldIm = Number(spec.centerIm ?? 0) - y * oldScale;
      onChange({ scale: nextScale, centerRe: worldRe - x * nextScale * aspect, centerIm: worldIm + y * nextScale });
    }}
    onDoubleClick={(event) => {
      const box = element.current?.getBoundingClientRect(); if (!box) return;
      onNavigationStart();
      const oldScale = Number(spec.scale ?? 3); const nextScale = Math.max(1e-12, oldScale * 0.35); const aspect = box.width / box.height;
      const x = (event.clientX - box.left) / box.width - 0.5; const y = (event.clientY - box.top) / box.height - 0.5;
      const worldRe = Number(spec.centerRe ?? 0) + x * oldScale * aspect; const worldIm = Number(spec.centerIm ?? 0) - y * oldScale;
      onChange({ scale: nextScale, centerRe: worldRe - x * nextScale * aspect, centerIm: worldIm + y * nextScale });
    }}
    style={{ touchAction: "none", aspectRatio: `${width}/${height}` }}>
    {preview ? <img src={preview} alt={labels.alt} draggable={false} className="h-full w-full select-none object-contain" /> : <div className="grid h-full min-h-[26rem] place-items-center text-sm text-white/45">{labels.empty}</div>}
    <div className="pointer-events-none absolute inset-0" aria-hidden="true">
      <span className="absolute left-1/2 top-0 h-full w-px bg-amber-300/10" />
      <span className="absolute left-0 top-1/2 h-px w-full bg-amber-300/10" />
      <span className="absolute left-1/2 top-1/2 h-2 w-2 -translate-x-1/2 -translate-y-1/2 border border-amber-300/45" />
    </div>
    <div className="pointer-events-none absolute inset-x-0 bottom-0 flex items-center justify-between bg-black/75 px-3 py-2 font-mono text-[11px] text-white/65">
      <span>{labels.hint}</span><span>{spec.iterations} {labels.detail}</span>
    </div>
    <span className="pointer-events-none absolute left-3 top-3 border border-amber-300/30 bg-black/75 px-2 py-1 font-mono text-[10px] uppercase tracking-wider text-amber-200/80">{labels.frame} {exportWidth}×{exportHeight}</span>
    {previewing && <div className="pointer-events-none absolute inset-0 grid place-items-center bg-black/25"><span className="flex items-center gap-2 border border-white/15 bg-black/80 px-3 py-1.5 text-sm"><LoaderCircle className="h-4 w-4 animate-spin" /> {labels.rendering}</span></div>}
    <div className="absolute right-3 top-3 flex overflow-hidden border border-white/20 bg-black/80">
      <Button aria-label={labels.zoomOut} title={labels.zoomOut} size="sm" variant="ghost" className="rounded-none" onClick={() => onZoom(2)}><Minus className="h-4 w-4" /></Button>
      <Button aria-label={labels.zoomIn} title={labels.zoomIn} size="sm" variant="ghost" className="rounded-none border-x border-white/15" onClick={() => onZoom(0.35)}><Plus className="h-4 w-4" /></Button>
      <Button aria-label={labels.reset} title={labels.reset} size="sm" variant="ghost" className="rounded-none" onClick={onReset}><RotateCcw className="h-4 w-4" /></Button>
    </div>
  </div>;
}
