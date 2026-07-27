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
};

export function InteractiveFractalCanvas({ spec, preview, previewing, width, height, onChange, onReset, onZoom, onNavigationStart }: Props) {
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

  return <div className="relative overflow-hidden rounded-2xl border border-white/10 bg-black shadow-2xl" ref={element}
    onPointerDown={(event) => {
      if ((event.target as HTMLElement).closest("button")) return;
      onNavigationStart(); event.currentTarget.setPointerCapture(event.pointerId);
      drag.current = { x: event.clientX, y: event.clientY, re: Number(spec.centerRe ?? 0), im: Number(spec.centerIm ?? 0) };
    }}
    onPointerMove={(event) => { const value = drag.current; if (value) move(event.clientX, event.clientY, value.re, value.im, value.x, value.y); }}
    onPointerUp={() => { drag.current = null; }}
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
    {preview ? <img src={preview} alt="Fractal preview" draggable={false} className="h-full w-full select-none object-contain" /> : <div className="grid h-full min-h-[26rem] place-items-center text-sm text-muted-foreground">Generating first preview…</div>}
    <div className="pointer-events-none absolute inset-x-0 bottom-0 flex items-center justify-between bg-gradient-to-t from-black/70 to-transparent p-3 text-xs text-white/70">
      <span>Drag to move · scroll, double-click or buttons to zoom</span><span>{spec.iterations} detail</span>
    </div>
    {previewing && <div className="pointer-events-none absolute inset-0 grid place-items-center bg-black/25"><span className="flex items-center gap-2 rounded-full bg-black/70 px-3 py-1.5 text-sm"><LoaderCircle className="h-4 w-4 animate-spin" /> Rendering preview</span></div>}
    <div className="absolute right-3 top-3 flex overflow-hidden rounded-lg border border-white/15 bg-black/70 backdrop-blur">
      <Button aria-label="Zoom out" size="sm" variant="ghost" className="rounded-none" onClick={() => onZoom(2)}><Minus className="h-4 w-4" /></Button>
      <Button aria-label="Zoom in" size="sm" variant="ghost" className="rounded-none border-x border-white/15" onClick={() => onZoom(0.35)}><Plus className="h-4 w-4" /></Button>
      <Button aria-label="Reset view" size="sm" variant="ghost" className="rounded-none" onClick={onReset}><RotateCcw className="h-4 w-4" /></Button>
    </div>
  </div>;
}
