"use client";

import { useEffect, useMemo, useRef, useState } from "react";
import type React from "react";
import { ChevronDown, Heart, Redo2, Save, Sparkles, Undo2, WandSparkles, X } from "lucide-react";
import { InteractiveFractalCanvas } from "@/components/studio/interactive-fractal-canvas";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { platform, type FractalSpec, type Recipe, type RenderJob, type StudioCapabilities } from "@/lib/api/platform";
import { jitterPreset, randomPreset, STUDIO_PRESETS } from "@/lib/studio-presets";

const defaults: FractalSpec = { version: 1, centerRe: -0.75, centerIm: 0, scale: 3, iterations: 256, variant: "mandelbrot", colorMap: "classic_cos", metric: "escape", smooth: false, rotationDeg: 0, pairwiseCap: 64, julia: false, bailout: 4, engine: "auto", scalarType: "auto" };
const fallbackCapabilities: StudioCapabilities = { metrics: ["escape", "min_abs", "max_abs", "envelope", "min_pairwise_dist", "mandel_ship_agree"], engines: ["auto", "openmp"], scalars: ["auto", "fp32", "fp64"], colorMaps: ["classic_cos", "mod17", "hsv_wheel", "tri765", "grayscale", "hs_rainbow", "inferno", "viridis", "twilight", "ember_blue", "spectral1530"], customGradient: { enabled: false, maxStops: 0 } };
const previewSizes = [{ name: "Square", width: 640, height: 640 }, { name: "Landscape", width: 768, height: 576 }, { name: "Wide", width: 768, height: 432 }, { name: "Portrait", width: 576, height: 768 }];
const renderSizes = [{ name: "Standard · 1024²", width: 1024, height: 1024 }, { name: "Large · 2048²", width: 2048, height: 2048 }, { name: "Wide · 1920×1080", width: 1920, height: 1080 }];
const quickPresets = STUDIO_PRESETS.slice(0, 6);
const quickPalettes = [
  { name: "Ocean", value: "viridis", color: "from-[#440154] via-[#21918c] to-[#fde725]" },
  { name: "Fire", value: "inferno", color: "from-[#000004] via-[#bc3754] to-[#fcffa4]" },
  { name: "Sunset", value: "ember_blue", color: "from-[#050820] via-[#37b4c3] to-[#fff6d2]" },
  { name: "Rainbow", value: "hsv_wheel", color: "from-red-500 via-emerald-400 to-violet-500" },
  { name: "Mono", value: "grayscale", color: "from-black to-white" },
];
const variantNames = [...new Set(STUDIO_PRESETS.map((item) => item.spec.variant).filter((item): item is string => Boolean(item)))];
const selectClass = "h-10 w-full rounded-lg border border-deep-slate bg-deep-slate/50 px-3 text-sm text-foreground focus:border-fractal-500 focus:outline-none";
type SavedView = { id: string; name: string; spec: FractalSpec };
function message(error: unknown): string { return error instanceof Error ? error.message : "Request failed"; }
function load<T>(key: string): T[] { try { return JSON.parse(localStorage.getItem(key) ?? "[]") as T[]; } catch { return []; } }
function selectValue<T extends string>(event: React.ChangeEvent<HTMLSelectElement>): T { return event.target.value as T; }

export default function StudioPage() {
  const [spec, setSpec] = useState<FractalSpec>(defaults);
  const [preview, setPreview] = useState<string | null>(null);
  const [previewing, setPreviewing] = useState(false);
  const [recipes, setRecipes] = useState<Recipe[]>([]);
  const [job, setJob] = useState<RenderJob | null>(null);
  const [capabilities, setCapabilities] = useState<StudioCapabilities>(fallbackCapabilities);
  const [previewSize, setPreviewSize] = useState(0);
  const [renderSize, setRenderSize] = useState(0);
  const [randomIntensity, setRandomIntensity] = useState(2);
  const [saved, setSaved] = useState<SavedView[]>([]);
  const [history, setHistory] = useState<SavedView[]>([]);
  const [undoStack, setUndoStack] = useState<FractalSpec[]>([]);
  const [redoStack, setRedoStack] = useState<FractalSpec[]>([]);
  const [error, setError] = useState<string | null>(null);
  const abortRef = useRef<AbortController | null>(null);
  const previewRef = useRef<string | null>(null);

  const canonical = useMemo<FractalSpec>(() => ({ ...spec, centerRe: Number(spec.centerRe), centerIm: Number(spec.centerIm), scale: Number(spec.scale), iterations: Math.max(1, Math.round(Number(spec.iterations))), bailout: Number(spec.bailout), rotationDeg: Number(spec.rotationDeg ?? 0), pairwiseCap: Math.max(1, Math.round(Number(spec.pairwiseCap ?? 64))) }), [spec]);
  const specKey = JSON.stringify(canonical);
  const activePreviewSize = previewSizes[previewSize]!;
  const zoomLevel = Math.max(0, Math.min(36, Math.round(Math.log2(3 / Number(spec.scale ?? 3)))));

  useEffect(() => {
    setSaved(load<SavedView>("fractal-studio-saved-v1")); setHistory(load<SavedView>("fractal-studio-history-v1"));
    void Promise.all([platform.studio.recipes(), platform.studio.capabilities()]).then(([recipePage, caps]) => { setRecipes(recipePage.data); setCapabilities(caps); }).catch((reason: unknown) => setError(message(reason)));
    return () => { abortRef.current?.abort(); if (previewRef.current) URL.revokeObjectURL(previewRef.current); };
  }, []);
  useEffect(() => {
    if (!job || ["completed", "failed", "cancelled"].includes(job.status)) return;
    const timer = window.setInterval(() => void platform.studio.job(job.id).then(setJob).catch((reason: unknown) => setError(message(reason))), 1500);
    return () => window.clearInterval(timer);
  }, [job]);
  useEffect(() => {
    abortRef.current?.abort(); const controller = new AbortController(); abortRef.current = controller;
    const timer = window.setTimeout(async () => {
      setPreviewing(true); setError(null);
      try {
        const blob = await platform.studio.preview(canonical, activePreviewSize.width, activePreviewSize.height, controller.signal);
        if (controller.signal.aborted) return;
        const url = URL.createObjectURL(blob); if (previewRef.current) URL.revokeObjectURL(previewRef.current); previewRef.current = url; setPreview(url);
        const entry = { id: crypto.randomUUID(), name: `${canonical.variant} · ${new Date().toLocaleTimeString()}`, spec: canonical };
        setHistory((current) => { const next = [entry, ...current.filter((item) => JSON.stringify(item.spec) !== specKey)].slice(0, 20); localStorage.setItem("fractal-studio-history-v1", JSON.stringify(next)); return next; });
      } catch (reason) { if (!controller.signal.aborted) setError(message(reason)); } finally { if (!controller.signal.aborted) setPreviewing(false); }
    }, 420);
    return () => { window.clearTimeout(timer); controller.abort(); };
  }, [specKey, previewSize]);

  const update = (patch: Partial<FractalSpec>) => setSpec((current) => ({ ...current, ...patch }));
  const rememberView = () => {
    setUndoStack((current) => JSON.stringify(current.at(-1)) === specKey ? current : [...current, canonical].slice(-40));
    setRedoStack([]);
  };
  const reset = () => { rememberView(); setSpec(defaults); };
  const zoom = (factor: number) => { rememberView(); update({ scale: Math.min(1e9, Math.max(1e-12, Number(spec.scale ?? 3) * factor)), iterations: Math.min(4000, Math.ceil(Number(spec.iterations ?? 256) * (factor < 1 ? 1.14 : 1))) }); };
  const setZoomLevel = (level: number) => { rememberView(); update({ scale: 3 / 2 ** level, iterations: Math.min(4000, Math.round(256 + level * 26)) }); };
  const undo = () => setUndoStack((current) => { const previous = current.at(-1); if (!previous) return current; setRedoStack((next) => [canonical, ...next].slice(0, 40)); setSpec(previous); return current.slice(0, -1); });
  const redo = () => setRedoStack((current) => { const next = current[0]; if (!next) return current; setUndoStack((previous) => [...previous, canonical].slice(-40)); setSpec(next); return current.slice(1); });
  const usePreset = (id: string) => { const preset = STUDIO_PRESETS.find((item) => item.id === id); if (preset) { rememberView(); setSpec({ ...defaults, ...preset.spec, version: 1 }); } };
  const randomize = () => { const preset = randomPreset(); rememberView(); setSpec({ ...defaults, ...jitterPreset(preset.spec, randomIntensity), version: 1 }); };
  const saveView = () => setSaved((current) => { const next = [{ id: crypto.randomUUID(), name: `View ${current.length + 1} · ${canonical.variant}`, spec: canonical }, ...current].slice(0, 20); localStorage.setItem("fractal-studio-saved-v1", JSON.stringify(next)); return next; });
  const dropSaved = (id: string) => setSaved((current) => { const next = current.filter((item) => item.id !== id); localStorage.setItem("fractal-studio-saved-v1", JSON.stringify(next)); return next; });
  const toggleGradient = (enabled: boolean) => update(enabled ? { colorMap: null, colorProgram: { schemaVersion: 1, type: "gradient", interpolation: "rgb", wrap: "repeat", cycles: 1, phase: 0, interiorColor: "#080b14", invalidColor: "#ff00ff", stops: [{ at: 0, color: "#16002d" }, { at: 0.45, color: "#00d4ff" }, { at: 1, color: "#ffe66d" }] } } : { colorProgram: null, colorMap: capabilities.colorMaps[0] ?? "classic_cos" });
  const updateStop = (index: number, color: string) => { const colorProgram = spec.colorProgram; if (colorProgram) update({ colorProgram: { ...colorProgram, stops: colorProgram.stops.map((stop, item) => item === index ? { ...stop, color } : stop) } }); };
  const saveAndRender = async () => { setError(null); try { const recipe = await platform.studio.createRecipe(canonical); setRecipes((current) => [recipe, ...current.filter((item) => item.id !== recipe.id)]); const size = renderSizes[renderSize]!; setJob(await platform.studio.createRender(recipe.id, { kind: "image", format: "png", width: size.width, height: size.height })); } catch (reason) { setError(message(reason)); } };

  return <div className="mx-auto max-w-[1500px] space-y-5">
    <header className="flex flex-wrap items-end justify-between gap-3"><div><p className="mb-1 text-xs font-medium uppercase tracking-[0.18em] text-fractal-300">Create a fractal</p><h1 className="text-3xl font-semibold">Explore, don’t configure.</h1><p className="mt-1 text-sm text-muted-foreground">Pick a look, zoom into details, drag image around. Every move makes a new picture.</p></div><div className="flex gap-2"><Button variant="outline" onClick={saveView}><Heart className="mr-2 h-4 w-4" />Save this view</Button><Button onClick={() => void saveAndRender()}><Save className="mr-2 h-4 w-4" />Make PNG</Button></div></header>
    <div className="grid gap-5 xl:grid-cols-[20rem_minmax(0,1fr)]">
      <aside className="space-y-4 rounded-2xl border border-white/10 bg-white/[0.025] p-4 xl:sticky xl:top-4 xl:self-start">
        <section><p className="mb-2 text-xs font-medium uppercase tracking-wider text-muted-foreground">1 · Start with a look</p><div className="grid grid-cols-2 gap-2">{quickPresets.map((item) => <Button key={item.id} size="sm" variant="outline" className="h-auto min-h-12 justify-start whitespace-normal text-left" onClick={() => usePreset(item.id)}>{item.name}</Button>)}</div><label className="mt-2 block text-xs text-muted-foreground"><span className="mb-1 block">More looks</span><select className={selectClass} defaultValue="" onChange={(e) => usePreset(e.target.value)}><option value="" disabled>Choose another look</option>{STUDIO_PRESETS.map((item) => <option value={item.id} key={item.id}>{item.name}</option>)}</select></label></section>
        <section><p className="mb-2 text-xs font-medium uppercase tracking-wider text-muted-foreground">2 · Change the mood</p><div className="grid grid-cols-5 gap-1">{quickPalettes.map((palette) => <button aria-label={`Use ${palette.name} colors`} className={`group h-9 rounded-md bg-gradient-to-r ${palette.color} ring-offset-2 transition hover:scale-105 focus:outline-none focus:ring-2 focus:ring-fractal-400`} key={palette.value} title={palette.name} onClick={() => { rememberView(); update({ colorProgram: null, colorMap: palette.value }); }}><span className="sr-only">{palette.name}</span></button>)}</div><p className="mt-1 text-xs text-muted-foreground">Color mood</p></section>
        <section><p className="mb-2 text-xs font-medium uppercase tracking-wider text-muted-foreground">3 · Add detail</p><div className="grid grid-cols-3 gap-1">{[{ name: "Soft", value: 160 }, { name: "Detailed", value: 512 }, { name: "Sharp", value: 1200 }].map((detail) => <Button key={detail.name} size="sm" variant={Number(spec.iterations) === detail.value ? "default" : "outline"} className="px-1" onClick={() => { rememberView(); update({ iterations: detail.value }); }}>{detail.name}</Button>)}</div><p className="mt-1 text-xs text-muted-foreground">More detail reveals finer edges.</p></section>
        <section className="rounded-xl border border-fractal-500/25 bg-fractal-500/5 p-3"><p className="mb-2 flex items-center gap-2 text-sm font-medium"><WandSparkles className="h-4 w-4 text-fractal-300" />2 · Surprise me</p><label className="mb-2 flex justify-between text-xs text-muted-foreground"><span>Variation</span><span>{["Subtle", "Light", "Playful", "Wild", "Chaos"][randomIntensity - 1]}</span></label><div className="flex items-center gap-2"><input aria-label="Randomize intensity" className="min-w-0 flex-1 accent-fractal-400" type="range" min="1" max="5" value={randomIntensity} onChange={(e) => setRandomIntensity(Number(e.target.value))} /><Button size="sm" onClick={randomize}><Sparkles className="mr-1 h-3.5 w-3.5" />Try</Button></div></section>
        <section className="rounded-xl border border-white/10 bg-black/15 p-3"><p className="font-medium">4 · Explore canvas</p><div className="mt-2 flex items-center gap-2"><Button aria-label="Go back" size="sm" variant="outline" disabled={!undoStack.length} onClick={undo}><Undo2 className="h-3.5 w-3.5" /></Button><Button aria-label="Go forward" size="sm" variant="outline" disabled={!redoStack.length} onClick={redo}><Redo2 className="h-3.5 w-3.5" /></Button><Button size="sm" variant="outline" onClick={() => zoom(2)}>Zoom out</Button><Button size="sm" onClick={() => zoom(0.35)}>Zoom in</Button></div><label className="mt-3 block text-xs text-muted-foreground"><span className="mb-1 flex justify-between"><span>Zoom depth</span><span>{zoomLevel}×</span></span><input aria-label="Zoom depth" className="w-full accent-fractal-400" type="range" min="0" max="36" value={zoomLevel} onChange={(event) => setZoomLevel(Number(event.target.value))} /></label><p className="mt-2 text-xs text-muted-foreground">Scroll, double-click or press +/−. Drag image to move.</p></section>
        <details className="rounded-xl border border-white/10 p-3"><summary className="flex cursor-pointer items-center justify-between text-sm font-medium">Fine tuning <ChevronDown className="h-4 w-4" /></summary><div className="mt-4 space-y-4"><section><p className="mb-2 text-xs font-medium uppercase tracking-wider text-muted-foreground">Look</p><Control label="Shape"><select className={selectClass} value={spec.variant} onChange={(e) => update({ variant: e.target.value })}>{variantNames.map((item) => <option key={item} value={item}>{item}</option>)}</select></Control><Control label="Colors"><select className={selectClass} value={spec.colorMap ?? ""} disabled={Boolean(spec.colorProgram)} onChange={(e) => update({ colorMap: e.target.value })}>{capabilities.colorMaps.map((item) => <option key={item}>{item}</option>)}</select></Control><label className="mt-2 flex items-center justify-between text-sm">Smooth colors <input type="checkbox" checked={Boolean(spec.smooth)} onChange={(e) => update({ smooth: e.target.checked })} /></label></section><section><p className="mb-2 text-xs font-medium uppercase tracking-wider text-muted-foreground">Canvas</p><div className="grid grid-cols-2 gap-2"><Control label="Frame"><select className={selectClass} value={previewSize} onChange={(e) => setPreviewSize(Number(e.target.value))}>{previewSizes.map((item, index) => <option value={index} key={item.name}>{item.name}</option>)}</select></Control><Control label="PNG size"><select className={selectClass} value={renderSize} onChange={(e) => setRenderSize(Number(e.target.value))}>{renderSizes.map((item, index) => <option value={index} key={item.name}>{item.name}</option>)}</select></Control></div></section><details className="rounded-lg bg-black/20 p-2"><summary className="cursor-pointer text-sm">Expert controls</summary><div className="mt-3 grid grid-cols-2 gap-2"><NumberControl label="Real" value={spec.centerRe ?? 0} step="0.000001" onChange={(value) => update({ centerRe: value })} /><NumberControl label="Imaginary" value={spec.centerIm ?? 0} step="0.000001" onChange={(value) => update({ centerIm: value })} /><NumberControl label="Scale" value={spec.scale ?? 3} min="0.000000000001" step="0.01" onChange={(value) => update({ scale: value })} /><NumberControl label="Iterations" value={spec.iterations ?? 256} min="1" max="4000" step="1" onChange={(value) => update({ iterations: value })} /><NumberControl label="Bailout" value={spec.bailout ?? 4} min="0.01" step="0.5" onChange={(value) => update({ bailout: value })} /><NumberControl label="Rotation°" value={spec.rotationDeg ?? 0} min="-360" max="360" step="1" onChange={(value) => update({ rotationDeg: value })} /><NumberControl label="Pair cap" value={spec.pairwiseCap ?? 64} min="1" max="1000000" step="1" onChange={(value) => update({ pairwiseCap: value })} /><Control label="Metric"><select className={selectClass} value={spec.metric} onChange={(e) => update({ metric: selectValue<NonNullable<FractalSpec["metric"]>>(e) })}>{capabilities.metrics.map((item) => <option key={item}>{item}</option>)}</select></Control><Control label="Engine"><select className={selectClass} value={spec.engine} onChange={(e) => update({ engine: selectValue<NonNullable<FractalSpec["engine"]>>(e) })}>{capabilities.engines.map((item) => <option key={item}>{item}</option>)}</select></Control><Control label="Scalar"><select className={selectClass} value={spec.scalarType} onChange={(e) => update({ scalarType: selectValue<NonNullable<FractalSpec["scalarType"]>>(e) })}>{capabilities.scalars.map((item) => <option key={item}>{item}</option>)}</select></Control><label className="col-span-2 flex items-center justify-between text-sm">Custom gradient <input type="checkbox" disabled={!capabilities.customGradient.enabled} checked={Boolean(spec.colorProgram)} onChange={(e) => toggleGradient(e.target.checked)} /></label>{spec.colorProgram && <div className="col-span-2"><GradientEditor program={spec.colorProgram} onChange={updateStop} /></div>}<label className="col-span-2 flex items-center justify-between text-sm">Julia mode <input type="checkbox" checked={Boolean(spec.julia)} onChange={(e) => update(e.target.checked ? { julia: true, juliaRe: spec.juliaRe ?? -0.8, juliaIm: spec.juliaIm ?? 0.156 } : { julia: false })} /></label>{spec.julia && <><NumberControl label="Julia real" value={spec.juliaRe ?? -0.8} step="0.0001" onChange={(value) => update({ juliaRe: value })} /><NumberControl label="Julia imaginary" value={spec.juliaIm ?? 0.156} step="0.0001" onChange={(value) => update({ juliaIm: value })} /></>}</div></details></div></details>
      </aside>
      <main className="min-w-0 space-y-4"><InteractiveFractalCanvas spec={canonical} preview={preview} previewing={previewing} width={activePreviewSize.width} height={activePreviewSize.height} onChange={update} onReset={reset} onZoom={zoom} onNavigationStart={rememberView} />{error && <p className="rounded-lg border border-red-500/30 bg-red-500/10 p-3 text-sm text-red-300">{error}</p>}{job && <div className="flex items-center justify-between rounded-xl border border-white/10 p-3 text-sm"><span>PNG: <b>{job.status}</b> · {job.progressPercent}%{job.assetId ? " · ready in Library" : ""}</span>{!["completed", "failed", "cancelled"].includes(job.status) && <Button size="sm" variant="outline" onClick={() => void platform.studio.cancel(job.id).then(setJob).catch((reason: unknown) => setError(message(reason)))}>Cancel</Button>}</div>}<div className="grid gap-4 lg:grid-cols-2"><ViewList title="Saved views" items={saved} onLoad={(item) => setSpec(item.spec)} onDrop={dropSaved} /><ViewList title="Recent explorations" items={history} onLoad={(item) => setSpec(item.spec)} /></div>{recipes.length > 0 && <section className="rounded-xl border border-white/10 p-4"><h2 className="mb-2 font-medium">Your rendered recipes</h2><div className="flex flex-wrap gap-2">{recipes.slice(0, 8).map((recipe) => <Button key={recipe.id} size="sm" variant="outline" onClick={() => setSpec(recipe.canonicalSpec)}>{String(recipe.canonicalSpec.variant)} · {String(recipe.canonicalSpec.colorMap ?? "custom colors")}</Button>)}</div></section>}</main>
    </div>
  </div>;
}

function Control({ label, children }: { label: string; children: React.ReactNode }) { return <label className="mb-2 block text-xs text-muted-foreground"><span className="mb-1 block">{label}</span>{children}</label>; }
function NumberControl({ label, value, onChange, ...props }: { label: string; value: number; onChange: (value: number) => void } & Omit<React.ComponentProps<typeof Input>, "value" | "onChange">) { return <Control label={label}><Input value={Number.isFinite(value) ? value : 0} type="number" onChange={(e) => onChange(Number(e.target.value))} {...props} /></Control>; }
function GradientEditor({ program, onChange }: { program: NonNullable<FractalSpec["colorProgram"]>; onChange: (index: number, color: string) => void }) { return <div className="space-y-2"><div className="h-5 rounded" style={{ background: `linear-gradient(90deg, ${program.stops.map((stop) => `${stop.color} ${stop.at * 100}%`).join(", ")})` }} />{program.stops.map((stop, index) => <label className="flex items-center justify-between text-xs" key={stop.at}><span>{Math.round(stop.at * 100)}%</span><input type="color" value={stop.color} onChange={(e) => onChange(index, e.target.value)} /></label>)}</div>; }
function ViewList({ title, items, onLoad, onDrop }: { title: string; items: SavedView[]; onLoad: (item: SavedView) => void; onDrop?: (id: string) => void }) { return <section className="rounded-2xl border border-white/10 bg-white/[0.025] p-4"><h2 className="mb-2 font-medium">{title}</h2><div className="space-y-2">{items.length ? items.map((item) => <div className="flex gap-1" key={item.id}><Button className="min-w-0 flex-1 justify-start truncate" size="sm" variant="outline" onClick={() => onLoad(item)}>{item.name}</Button>{onDrop && <Button aria-label={`Remove ${item.name}`} size="icon" variant="ghost" onClick={() => onDrop(item.id)}><X className="h-3.5 w-3.5" /></Button>}</div>) : <p className="text-sm text-muted-foreground">Save a view you want to revisit.</p>}</div></section>; }
