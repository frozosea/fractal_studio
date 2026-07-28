"use client";

import { useEffect, useMemo, useRef, useState } from "react";
import type React from "react";
import {
  Braces,
  CheckCircle2,
  Download,
  ExternalLink,
  ImageDown,
  Layers3,
  Plus,
  Save,
  Trash2,
} from "lucide-react";
import { useTranslations } from "next-intl";
import { InteractiveFractalCanvas } from "@/components/studio/interactive-fractal-canvas";
import { useAuth } from "@/providers/auth-provider";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { Link } from "@/i18n/navigation";
import {
  platform,
  PlatformApiError,
  type FractalSpec,
  type OrbitProgram,
  type RenderJob,
  type StudioCapabilities,
} from "@/lib/api/platform";
import {
  AXIS_TRANSITION_VARIANTS,
  BUILTIN_VARIANTS,
  COLOR_MAPS,
  FORMULA_EXAMPLES,
  LOCATION_PRESETS,
  METRICS,
  OUTPUT_PRESETS,
  TRANSITION_METRICS,
  previewDimensions,
} from "@/lib/studio-catalog";

type ImageMode = "map" | "julia" | "transitionPair" | "transitionMulti" | "formula" | "sequence";
type SequenceOrbit = Extract<OrbitProgram, { type: "sequence" }>;

const defaults: FractalSpec = {
  version: 1,
  centerRe: -0.75,
  centerIm: 0,
  centerReStr: "-0.75",
  centerImStr: "0",
  scale: 3,
  iterations: 512,
  variant: "mandelbrot",
  colorMap: "spectral1530",
  metric: "escape",
  smooth: false,
  colorMode: "eq_full",
  cyclesPerOctave: 0.01,
  rotationDeg: 0,
  pairwiseCap: 64,
  julia: false,
  bailout: 2,
  engine: "auto",
  scalarType: "auto",
  orbitProgram: null,
  transitionMode: "off",
  transitionThetaMilliDeg: 0,
  transitionFrom: "mandelbrot",
  transitionTo: "burning_ship",
  transitionLegs: [
    { variant: "mandelbrot", weight: 1 },
    { variant: "burning_ship", weight: 1 },
  ],
};

const fallbackCapabilities: StudioCapabilities = {
  metrics: [...METRICS],
  engines: ["auto", "openmp"],
  scalars: ["auto", "fp32", "fp64"],
  colorMaps: COLOR_MAPS.map((item) => item.id),
  colorModes: ["direct", "eq_full", "eq_center"],
  variants: [...BUILTIN_VARIANTS],
  axisTransitionVariants: [...AXIS_TRANSITION_VARIANTS],
  imageKinds: {
    map: { enabled: true, metrics: [...METRICS], engines: ["auto", "openmp"], scalars: ["auto", "fp32", "fp64"], orbitProgram: true },
    transition: { enabled: false, metrics: [...TRANSITION_METRICS], engines: ["auto", "openmp"], scalars: ["auto", "fp32", "fp64"], orbitProgram: false },
  },
  orbitPrograms: { formula: true, sequence: true },
  customGradient: { enabled: false, maxStops: 0, kinds: [] },
};

const previewMinIntervalMs = 2100;
const juliaPreviewMinIntervalMs = 4300;
const previewDebounceMs = 600;
const minScale = 3 / 2 ** 41;
const selectClass = "instrument-control h-9 w-full px-2 text-sm";
const terminalStatuses = new Set(["completed", "failed", "cancelled"]);

function errorCode(error: unknown): string | null {
  return error instanceof PlatformApiError ? error.code : null;
}

function isTransitionMode(mode: ImageMode): boolean {
  return mode === "transitionPair" || mode === "transitionMulti";
}

function formulaProgram(source = "z*z+c"): Extract<OrbitProgram, { type: "formula" }> {
  const normalized = source.replace(/\s+/g, "").toLowerCase();
  const example = FORMULA_EXAMPLES.find((item) => item.source.replace(/\s+/g, "").toLowerCase() === normalized);
  if (example) {
    return { type: "formula", formula: { type: "builtin", id: example.id } };
  }
  if (normalized === "z^2+c" || normalized === "pow(z,2)+c") {
    return { type: "formula", formula: { type: "builtin", id: "mandelbrot" } };
  }
  if (normalized === "conj(z)*conj(z)+c" || normalized === "conj(z)^2+c" || normalized === "pow(conj(z),2)+c") {
    return { type: "formula", formula: { type: "builtin", id: "tricorn" } };
  }
  return { type: "formula", formula: { type: "dsl", source } };
}

function formulaSource(program: Extract<OrbitProgram, { type: "formula" }>): string {
  const formula = program.formula;
  if (formula.type === "dsl") return formula.source;
  return FORMULA_EXAMPLES.find((item) => item.id === formula.id)?.source ?? "z*z+c";
}

function sequenceProgram(): SequenceOrbit {
  return {
    type: "sequence",
    repeat: true,
    steps: [
      { span: 2, program: formulaProgram("z*z+c") },
      { span: 1, program: formulaProgram("conj(z)*conj(z)+c") },
    ],
  };
}

function modeLabelKey(mode: ImageMode): string {
  return `modes.${mode}`;
}

function defaultBailoutForVariant(variant: string): number {
  return ["sin_z", "cos_z", "exp_z", "sinh_z", "cosh_z", "tan_z"].includes(variant) ? 64 : 2;
}

function formatCoordinate(value: number | undefined): string {
  const numeric = Number(value ?? 0);
  return Number.isFinite(numeric) ? numeric.toPrecision(12).replace(/(?:\.0+|(?:(\.\d*?)0+))(?=e|$)/, "$1") : "0";
}

/** Modes that require membership — gated for non-members */
const MEMBER_ONLY_MODES: ReadonlySet<ImageMode> = new Set(["transitionMulti", "formula", "sequence"]);

export default function StudioPage() {
  const t = useTranslations("studio");
  const { user } = useAuth();
  const isMember = !!user?.member;
  const [spec, setSpec] = useState<FractalSpec>(defaults);
  const [juliaPickerSpec, setJuliaPickerSpec] = useState<FractalSpec>(defaults);
  const [mode, setMode] = useState<ImageMode>("map");
  const [preview, setPreview] = useState<string | null>(null);
  const [juliaPickerPreview, setJuliaPickerPreview] = useState<string | null>(null);
  const [previewing, setPreviewing] = useState(false);
  const [capabilities, setCapabilities] = useState<StudioCapabilities>(fallbackCapabilities);
  const [output, setOutput] = useState({ preset: "square", width: 1024, height: 1024 });
  const [job, setJob] = useState<RenderJob | null>(null);
  const [exporting, setExporting] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [notice, setNotice] = useState<string | null>(null);
  const [retryTick, setRetryTick] = useState(0);
  const abortRef = useRef<AbortController | null>(null);
  const previewRef = useRef<string | null>(null);
  const juliaPickerPreviewRef = useRef<string | null>(null);
  const lastPreviewAtRef = useRef(0);

  const previewSize = useMemo(() => previewDimensions(output.width, output.height), [output.height, output.width]);
  const canonical = useMemo<FractalSpec>(() => {
    const centerRe = Number(spec.centerReStr ?? spec.centerRe ?? 0);
    const centerIm = Number(spec.centerImStr ?? spec.centerIm ?? 0);
    return {
      ...spec,
      centerRe: Number.isFinite(centerRe) ? centerRe : Number(spec.centerRe ?? 0),
      centerIm: Number.isFinite(centerIm) ? centerIm : Number(spec.centerIm ?? 0),
      scale: Math.max(minScale, Number(spec.scale ?? 3)),
      iterations: Math.max(1, Math.round(Number(spec.iterations ?? 512))),
      smooth: spec.metric === "escape" ? false : Boolean(spec.smooth),
      bailout: Math.max(0.01, Number(spec.bailout ?? 2)),
      cyclesPerOctave: Math.max(0.01, Math.min(64, Number(spec.cyclesPerOctave ?? 1))),
      rotationDeg: Number(spec.rotationDeg ?? 0),
      pairwiseCap: Math.max(1, Math.round(Number(spec.pairwiseCap ?? 64))),
      transitionThetaMilliDeg: Math.round(Number(spec.transitionThetaMilliDeg ?? 0)),
    };
  }, [spec]);
  const juliaPickerCanonical = useMemo<FractalSpec>(() => {
    const { juliaRe: _juliaRe, juliaIm: _juliaIm, ...shared } = canonical;
    const centerRe = Number(juliaPickerSpec.centerReStr ?? juliaPickerSpec.centerRe ?? 0);
    const centerIm = Number(juliaPickerSpec.centerImStr ?? juliaPickerSpec.centerIm ?? 0);
    return {
      ...shared,
      centerRe: Number.isFinite(centerRe) ? centerRe : Number(juliaPickerSpec.centerRe ?? 0),
      centerIm: Number.isFinite(centerIm) ? centerIm : Number(juliaPickerSpec.centerIm ?? 0),
      centerReStr: juliaPickerSpec.centerReStr ?? String(juliaPickerSpec.centerRe ?? 0),
      centerImStr: juliaPickerSpec.centerImStr ?? String(juliaPickerSpec.centerIm ?? 0),
      scale: Math.max(minScale, Number(juliaPickerSpec.scale ?? 3)),
      julia: false,
      transitionMode: "off",
      orbitProgram: null,
      metric: "escape",
      smooth: false,
    };
  }, [canonical, juliaPickerSpec]);
  const specKey = JSON.stringify(canonical);
  const juliaPickerSpecKey = JSON.stringify(juliaPickerCanonical);
  const zoomLevel = Math.max(0, Math.log2(3 / Number(canonical.scale ?? 3)));
  const availableVariants = capabilities.variants.length ? capabilities.variants : [...BUILTIN_VARIANTS];
  const axisVariants = capabilities.axisTransitionVariants.length ? capabilities.axisTransitionVariants : [...AXIS_TRANSITION_VARIANTS];
  const activeMetrics = isTransitionMode(mode)
    ? (capabilities.imageKinds.transition.metrics.length ? capabilities.imageKinds.transition.metrics : [...TRANSITION_METRICS])
    : (capabilities.imageKinds.map.metrics.length ? capabilities.imageKinds.map.metrics : [...METRICS]);
  const selectedColor = COLOR_MAPS.find((item) => item.id === spec.colorMap) ?? COLOR_MAPS[0];

  useEffect(() => {
    void platform.studio.capabilities().then(setCapabilities).catch((reason: unknown) => {
      setError(`${t("errors.capabilities")} (${errorCode(reason) ?? "request_failed"})`);
    });
    return () => {
      abortRef.current?.abort();
      if (previewRef.current) URL.revokeObjectURL(previewRef.current);
      if (juliaPickerPreviewRef.current) URL.revokeObjectURL(juliaPickerPreviewRef.current);
    };
  }, [t]);

  useEffect(() => {
    if (!job || terminalStatuses.has(job.status)) return;
    const timer = window.setInterval(() => {
      void platform.studio.job(job.id).then(setJob).catch((reason: unknown) => {
        setError(`${t("errors.job")} (${errorCode(reason) ?? "request_failed"})`);
      });
    }, 1500);
    return () => window.clearInterval(timer);
  }, [job, t]);

  useEffect(() => {
    abortRef.current?.abort();
    const controller = new AbortController();
    abortRef.current = controller;
    const minimumInterval = mode === "julia" ? juliaPreviewMinIntervalMs : previewMinIntervalMs;
    const wait = Math.max(previewDebounceMs, lastPreviewAtRef.current + minimumInterval - Date.now());
    const timer = window.setTimeout(async () => {
      setPreviewing(true);
      setError(null);
      setNotice(null);
      lastPreviewAtRef.current = Date.now();
      try {
        const [blob, pickerBlob] = await Promise.all([
          platform.studio.preview(canonical, previewSize.width, previewSize.height, controller.signal),
          ...(mode === "julia" ? [platform.studio.preview(juliaPickerCanonical, previewSize.width, previewSize.height, controller.signal)] : []),
        ]);
        if (controller.signal.aborted) return;
        const url = URL.createObjectURL(blob);
        if (previewRef.current) URL.revokeObjectURL(previewRef.current);
        previewRef.current = url;
        setPreview(url);
        if (pickerBlob) {
          const pickerUrl = URL.createObjectURL(pickerBlob);
          if (juliaPickerPreviewRef.current) URL.revokeObjectURL(juliaPickerPreviewRef.current);
          juliaPickerPreviewRef.current = pickerUrl;
          setJuliaPickerPreview(pickerUrl);
        }
      } catch (reason) {
        if (controller.signal.aborted) return;
        if (reason instanceof PlatformApiError && reason.status === 429) {
          lastPreviewAtRef.current = Date.now() + minimumInterval;
          setNotice(t("errors.previewRate"));
          setRetryTick((current) => current + 1);
        } else {
          setError(`${t("errors.preview")} (${errorCode(reason) ?? "request_failed"})`);
        }
      } finally {
        if (!controller.signal.aborted) setPreviewing(false);
      }
    }, wait);
    return () => {
      window.clearTimeout(timer);
      controller.abort();
    };
  }, [canonical, juliaPickerCanonical, juliaPickerSpecKey, mode, previewSize.height, previewSize.width, retryTick, specKey, t]);

  const update = (patch: Partial<FractalSpec>) => setSpec((current) => ({ ...current, ...patch }));
  const updateViewport = (patch: Partial<FractalSpec>) => {
    setSpec((current) => ({
      ...current,
      ...patch,
      ...(patch.centerRe === undefined ? {} : { centerReStr: String(patch.centerRe) }),
      ...(patch.centerIm === undefined ? {} : { centerImStr: String(patch.centerIm) }),
    }));
  };
  const updateJuliaPickerViewport = (patch: Partial<FractalSpec>) => {
    setJuliaPickerSpec((current) => ({
      ...current,
      ...patch,
      ...(patch.centerRe === undefined ? {} : { centerReStr: String(patch.centerRe) }),
      ...(patch.centerIm === undefined ? {} : { centerImStr: String(patch.centerIm) }),
    }));
  };

  const selectMode = (next: ImageMode) => {
    setMode(next);
    if (next === "map") {
      if (mode === "julia") {
        setSpec((current) => ({
          ...current,
          centerRe: juliaPickerSpec.centerRe,
          centerIm: juliaPickerSpec.centerIm,
          centerReStr: juliaPickerSpec.centerReStr,
          centerImStr: juliaPickerSpec.centerImStr,
          scale: juliaPickerSpec.scale,
          julia: false,
          transitionMode: "off",
          orbitProgram: null,
          metric: "escape",
          smooth: false,
        }));
      } else update({ julia: false, transitionMode: "off", orbitProgram: null, metric: "escape", smooth: false });
    }
    if (next === "julia" && mode !== "julia") {
      const selectedRe = spec.juliaRe ?? Number(spec.centerReStr ?? spec.centerRe ?? -0.8);
      const selectedIm = spec.juliaIm ?? Number(spec.centerImStr ?? spec.centerIm ?? 0.156);
      setJuliaPickerSpec({ ...spec, julia: false, transitionMode: "off", orbitProgram: null, metric: "escape", smooth: false });
      setSpec((current) => ({
        ...current,
        centerRe: 0,
        centerIm: 0,
        centerReStr: "0",
        centerImStr: "0",
        scale: 3,
        julia: true,
        juliaRe: selectedRe,
        juliaIm: selectedIm,
        transitionMode: "off",
        orbitProgram: null,
        metric: "escape",
        smooth: false,
      }));
    }
    if (next === "transitionPair") update({ julia: false, transitionMode: "pair", orbitProgram: null, metric: "escape" });
    if (next === "transitionMulti") update({ julia: false, transitionMode: "multi", transitionThetaMilliDeg: 0, orbitProgram: null, metric: "escape" });
    if (next === "formula") update({ julia: false, transitionMode: "off", orbitProgram: formulaProgram(), metric: "escape" });
    if (next === "sequence") update({ julia: false, transitionMode: "off", orbitProgram: sequenceProgram(), metric: "escape" });
  };

  const reset = () => {
    setMode("map");
    setSpec(defaults);
    setJuliaPickerSpec(defaults);
  };
  const zoom = (factor: number) => updateViewport({
    scale: Math.min(1e9, Math.max(minScale, Number(spec.scale ?? 3) * factor)),
    iterations: Math.min(20_000, Math.ceil(Number(spec.iterations ?? 512) * (factor < 1 ? 1.12 : 1))),
  });
  const zoomJuliaPicker = (factor: number) => updateJuliaPickerViewport({
    scale: Math.min(1e9, Math.max(minScale, Number(juliaPickerSpec.scale ?? 3) * factor)),
  });
  const resetJuliaViewport = () => updateViewport({ centerRe: 0, centerIm: 0, scale: 3 });
  const resetJuliaPickerViewport = () => updateJuliaPickerViewport({ centerRe: -0.75, centerIm: 0, scale: 3 });
  const useLocation = (id: string) => {
    const location = LOCATION_PRESETS.find((item) => item.id === id);
    if (!location) return;
    const centerRe = Number(location.spec.centerRe ?? -0.75);
    const centerIm = Number(location.spec.centerIm ?? 0);
    update({ ...location.spec, centerReStr: String(centerRe), centerImStr: String(centerIm) });
  };

  const setFormulaSource = (source: string) => update({ orbitProgram: formulaProgram(source) });
  const sequence = spec.orbitProgram?.type === "sequence" ? spec.orbitProgram : sequenceProgram();
  const updateSequenceStep = (index: number, patch: { source?: string; span?: number }) => {
    const steps = sequence.steps.map((step, item) => item === index ? {
      ...step,
      span: patch.span ?? step.span,
      program: patch.source === undefined ? step.program : formulaProgram(patch.source),
    } : step);
    update({ orbitProgram: { ...sequence, steps } });
  };
  const addSequenceStep = () => {
    if (sequence.steps.length >= 4) return;
    update({ orbitProgram: { ...sequence, steps: [...sequence.steps, { span: 1, program: formulaProgram() }] } });
  };
  const removeSequenceStep = (index: number) => {
    if (sequence.steps.length <= 1) return;
    update({ orbitProgram: { ...sequence, steps: sequence.steps.filter((_, item) => item !== index) } });
  };

  const updateTransitionLeg = (index: number, patch: Partial<{ variant: string; weight: number }>) => {
    const legs = (spec.transitionLegs ?? defaults.transitionLegs ?? []).map((leg, item) => item === index ? { ...leg, ...patch } : leg);
    update({ transitionLegs: legs });
  };
  const addTransitionLeg = () => {
    const legs = spec.transitionLegs ?? [];
    if (legs.length >= 4) return;
    update({ transitionLegs: [...legs, { variant: axisVariants[legs.length % axisVariants.length] ?? "mandelbrot", weight: 1 }] });
  };
  const removeTransitionLeg = (index: number) => {
    const legs = spec.transitionLegs ?? [];
    if (legs.length <= 1) return;
    update({ transitionLegs: legs.filter((_, item) => item !== index) });
  };

  const toggleGradient = (enabled: boolean) => update(enabled ? {
    colorMap: null,
    colorMode: "direct",
    colorProgram: {
      schemaVersion: 1,
      type: "gradient",
      interpolation: "rgb",
      wrap: "repeat",
      cycles: 1,
      phase: 0,
      interiorColor: "#050505",
      invalidColor: "#ff00ff",
      stops: [
        { at: 0, color: "#071426" },
        { at: 0.5, color: "#2a7193" },
        { at: 1, color: "#f0a030" },
      ],
    },
  } : { colorProgram: null, colorMap: capabilities.colorMaps.includes("spectral1530") ? "spectral1530" : capabilities.colorMaps[0] ?? "classic_cos" });
  const updateGradient = (patch: Partial<NonNullable<FractalSpec["colorProgram"]>>) => {
    if (spec.colorProgram) update({ colorProgram: { ...spec.colorProgram, ...patch } });
  };
  const updateGradientStop = (index: number, patch: Partial<{ at: number; color: string }>) => {
    if (!spec.colorProgram) return;
    const stops = spec.colorProgram.stops.map((stop, item) => item === index ? { ...stop, ...patch } : stop);
    updateGradient({ stops });
  };
  const addGradientStop = () => {
    if (!spec.colorProgram || spec.colorProgram.stops.length >= capabilities.customGradient.maxStops) return;
    let gapIndex = 0;
    for (let index = 1; index < spec.colorProgram.stops.length; index += 1) {
      const gap = spec.colorProgram.stops[index]!.at - spec.colorProgram.stops[index - 1]!.at;
      const largest = spec.colorProgram.stops[gapIndex + 1]!.at - spec.colorProgram.stops[gapIndex]!.at;
      if (gap > largest) gapIndex = index - 1;
    }
    const left = spec.colorProgram.stops[gapIndex]!;
    const right = spec.colorProgram.stops[gapIndex + 1]!;
    const stops = [
      ...spec.colorProgram.stops.slice(0, gapIndex + 1),
      { at: (left.at + right.at) / 2, color: "#f0a030" },
      ...spec.colorProgram.stops.slice(gapIndex + 1),
    ];
    updateGradient({ stops });
  };
  const removeGradientStop = (index: number) => {
    if (!spec.colorProgram || spec.colorProgram.stops.length <= 2) return;
    updateGradient({ stops: spec.colorProgram.stops.filter((_, item) => item !== index) });
  };

  const chooseOutputPreset = (id: string) => {
    const preset = OUTPUT_PRESETS.find((item) => item.id === id);
    if (preset) setOutput({ preset: id, width: preset.width, height: preset.height });
    else setOutput((current) => ({ ...current, preset: "custom" }));
  };
  const updateOutputDimension = (field: "width" | "height", value: number) => {
    setOutput((current) => ({ ...current, preset: "custom", [field]: Math.max(64, Math.min(4096, Math.round(value))) }));
  };
  const saveAndRender = async () => {
    setError(null);
    setExporting(true);
    try {
      const recipe = await platform.studio.createRecipe(canonical);
      setJob(await platform.studio.createRender(recipe.id, {
        kind: "image",
        format: "png",
        width: output.width,
        height: output.height,
      }));
    } catch (reason) {
      setError(`${t("errors.export")} (${errorCode(reason) ?? "request_failed"})`);
    } finally {
      setExporting(false);
    }
  };
  const cancelJob = async () => {
    if (!job) return;
    try {
      setJob(await platform.studio.cancel(job.id));
    } catch (reason) {
      setError(`${t("errors.cancel")} (${errorCode(reason) ?? "request_failed"})`);
    }
  };
  const downloadAsset = async () => {
    if (!job?.assetId) return;
    try {
      window.open((await platform.assets.downloadUrl(job.assetId)).url, "_blank", "noopener,noreferrer");
    } catch (reason) {
      setError(`${t("errors.download")} (${errorCode(reason) ?? "request_failed"})`);
    }
  };

  return (
    <div className="scientific-studio mx-auto max-w-[1560px] space-y-4">
      <header className="flex flex-wrap items-end justify-between gap-4 border-b border-white/10 pb-4">
        <div>
          <p className="instrument-kicker">{t("eyebrow")}</p>
          <h1 className="mt-1 text-2xl font-medium tracking-tight text-white">{t("title")}</h1>
          <p className="mt-1 max-w-3xl text-sm text-white/50">{t("subtitle")}</p>
        </div>
        <div className="flex items-center gap-2 font-mono text-[11px] text-white/45">
          <span className="h-1.5 w-1.5 bg-emerald-400" />
          {t("renderer")}: {capabilities.rendererVersion ?? t("loadingCapabilities")}
        </div>
      </header>

      <div className="grid gap-4 xl:grid-cols-[23rem_minmax(0,1fr)]">
        <aside className="space-y-3 xl:sticky xl:top-4 xl:max-h-[calc(100vh-7rem)] xl:overflow-y-auto xl:pr-1">
          <Panel index="01" title={t("sections.imageMode")}>
            <div className="grid grid-cols-2 gap-1.5">
              {(["map", "julia", "transitionPair", "transitionMulti", "formula", "sequence"] as const).map((item) => {
                const locked = !isMember && MEMBER_ONLY_MODES.has(item);
                const disabled = locked
                  || (item.startsWith("transition") && !capabilities.imageKinds.transition.enabled)
                  || (item === "formula" && capabilities.orbitPrograms.formula === false)
                  || (item === "sequence" && capabilities.orbitPrograms.sequence === false);
                return (
                  <button
                    className="instrument-mode"
                    data-active={mode === item}
                    disabled={disabled}
                    key={item}
                    onClick={() => selectMode(item)}
                    type="button"
                    title={locked ? t("memberLocked") : undefined}
                  >
                    {item === "formula" ? <Braces className="h-3.5 w-3.5" /> : item === "sequence" ? <Layers3 className="h-3.5 w-3.5" /> : null}
                    {t(modeLabelKey(item))}
                    {locked && <span className="ml-1 text-[10px] opacity-60">🔒</span>}
                  </button>
                );
              })}
            </div>
            <p className="mt-2 text-xs leading-relaxed text-white/40">{t(`modeNotes.${mode}`)}</p>
          </Panel>

          <Panel index="02" title={t("sections.geometry")}>
            {(mode === "map" || mode === "julia") && (
              <>
                <Control label={t("variant")}>
                  <select className={selectClass} value={spec.variant} onChange={(event) => update({ variant: event.target.value, bailout: defaultBailoutForVariant(event.target.value) })}>
                    {availableVariants.map((item) => <option key={item} value={item}>{t(`variants.${item}.name`)}</option>)}
                  </select>
                </Control>
                <p className="instrument-note">{t(`variants.${spec.variant ?? "mandelbrot"}.description`)}</p>
                {mode === "map" && (
                  <Control label={t("locationPreset")}>
                    <select className={selectClass} defaultValue="" onChange={(event) => useLocation(event.target.value)}>
                      <option disabled value="">{t("chooseLocation")}</option>
                      {LOCATION_PRESETS.map((item) => <option key={item.id} value={item.id}>{t(`locations.${item.id}`)}</option>)}
                    </select>
                  </Control>
                )}
              </>
            )}

            {mode === "julia" && (
              <>
                <div className="grid grid-cols-2 gap-2">
                  <NumberControl label={t("juliaRe")} value={spec.juliaRe ?? -0.8} step="0.0001" onChange={(value) => update({ juliaRe: value })} />
                  <NumberControl label={t("juliaIm")} value={spec.juliaIm ?? 0.156} step="0.0001" onChange={(value) => update({ juliaIm: value })} />
                </div>
                <p className="instrument-note">{t("juliaSelectInstruction")}</p>
              </>
            )}

            {mode === "formula" && (
              <>
                <Control label={t("formulaSource")}>
                  <textarea
                    className="instrument-control min-h-24 w-full resize-y p-2 font-mono text-xs"
                    spellCheck={false}
                    value={spec.orbitProgram?.type === "formula" ? formulaSource(spec.orbitProgram) : "z*z+c"}
                    onChange={(event) => setFormulaSource(event.target.value)}
                  />
                </Control>
                <details className="mb-2 border border-white/10 bg-black/15">
                  <summary className="flex cursor-pointer list-none items-center justify-between px-2 py-1.5 text-[11px] uppercase tracking-wider text-white/50 hover:text-white/75">
                    <span>{t("formulaExamples")}</span>
                    <span className="font-mono text-amber-200/60">{FORMULA_EXAMPLES.length}</span>
                  </summary>
                  <div className="max-h-64 space-y-1 overflow-y-auto border-t border-white/10 p-1.5">
                    {FORMULA_EXAMPLES.map((item) => {
                      const name = t(`variants.${item.id}.name`);
                      return (
                        <button
                          aria-label={t("useFormulaExample", { name })}
                          className="block w-full border border-transparent px-2 py-1.5 text-left hover:border-amber-300/25 hover:bg-amber-400/5"
                          key={item.id}
                          onClick={() => setFormulaSource(item.source)}
                          type="button"
                        >
                          <span className="block text-[11px] text-white/65">{name}</span>
                          <code className="mt-0.5 block break-all text-[10px] leading-relaxed text-amber-100/65">{item.source}</code>
                        </button>
                      );
                    })}
                  </div>
                  <p className="border-t border-white/10 px-2 py-1.5 text-[10px] leading-relaxed text-white/35">{t("formulaExamplesHint")}</p>
                </details>
                <p className="instrument-note">{t("formulaHint")}</p>
              </>
            )}

            {mode === "sequence" && (
              <div className="space-y-2">
                {sequence.steps.map((step, index) => (
                  <div className="border border-white/10 bg-black/20 p-2" key={index}>
                    <div className="mb-2 flex items-center justify-between">
                      <span className="font-mono text-[10px] uppercase tracking-wider text-amber-200/70">{t("sequenceStep", { index: index + 1 })}</span>
                      <button aria-label={t("removeStep")} className="text-white/35 hover:text-red-300 disabled:opacity-20" disabled={sequence.steps.length <= 1} onClick={() => removeSequenceStep(index)} type="button"><Trash2 className="h-3.5 w-3.5" /></button>
                    </div>
                    <textarea
                      className="instrument-control min-h-16 w-full resize-y p-2 font-mono text-xs"
                      spellCheck={false}
                      value={formulaSource(step.program)}
                      onChange={(event) => updateSequenceStep(index, { source: event.target.value })}
                    />
                    <div className="mt-2 w-28">
                      <NumberControl label={t("sequenceSpan")} min="1" max="1000000" step="1" value={step.span} onChange={(value) => updateSequenceStep(index, { span: Math.max(1, Math.round(value)) })} />
                    </div>
                  </div>
                ))}
                <Button className="w-full rounded-none" disabled={sequence.steps.length >= 4} size="sm" variant="outline" onClick={addSequenceStep}><Plus className="h-3.5 w-3.5" />{t("addStep")}</Button>
                <p className="instrument-note">{t("sequenceHint")}</p>
              </div>
            )}

            {isTransitionMode(mode) && (
              <>
                {mode === "transitionPair" && (
                  <div className="mb-3">
                    <label className="mb-1 flex justify-between text-xs text-white/50"><span>{t("transitionAngle")}</span><span className="font-mono text-amber-200/80">{((spec.transitionThetaMilliDeg ?? 0) / 1000).toFixed(1)}°</span></label>
                    <div className="grid grid-cols-[minmax(0,1fr)_6rem] items-center gap-2">
                      <input className="w-full accent-amber-500" max="180000" min="-180000" step="1000" type="range" value={spec.transitionThetaMilliDeg ?? 0} onChange={(event) => update({ transitionThetaMilliDeg: Number(event.target.value) })} />
                      <Input
                        aria-label={t("transitionAngleInput")}
                        className="h-9 rounded-none font-mono text-xs"
                        max="180"
                        min="-180"
                        step="0.1"
                        type="number"
                        value={(spec.transitionThetaMilliDeg ?? 0) / 1000}
                        onChange={(event) => {
                          const degrees = Number(event.target.value);
                          if (Number.isFinite(degrees)) update({ transitionThetaMilliDeg: Math.round(Math.max(-180, Math.min(180, degrees)) * 1000) });
                        }}
                      />
                    </div>
                  </div>
                )}
                {mode === "transitionPair" ? (
                  <div className="grid grid-cols-2 gap-2">
                    <Control label={t("transitionFrom")}>
                      <select className={selectClass} value={spec.transitionFrom} onChange={(event) => update({ transitionFrom: event.target.value })}>{axisVariants.map((item) => <option key={item} value={item}>{t(`variants.${item}.name`)}</option>)}</select>
                    </Control>
                    <Control label={t("transitionTo")}>
                      <select className={selectClass} value={spec.transitionTo} onChange={(event) => update({ transitionTo: event.target.value })}>{axisVariants.map((item) => <option key={item} value={item}>{t(`variants.${item}.name`)}</option>)}</select>
                    </Control>
                  </div>
                ) : (
                  <div className="space-y-2">
                    {(spec.transitionLegs ?? []).map((leg, index) => (
                      <div className="grid grid-cols-[minmax(0,1fr)_5.5rem_1.5rem] items-end gap-1.5" key={index}>
                        <Control label={t("transitionLeg", { index: index + 1 })}>
                          <select className={selectClass} value={leg.variant} onChange={(event) => updateTransitionLeg(index, { variant: event.target.value })}>{axisVariants.map((item) => <option key={item} value={item}>{t(`variants.${item}.name`)}</option>)}</select>
                        </Control>
                        <NumberControl label={t("weight")} min="0.01" max="1000000" step="0.05" value={leg.weight} onChange={(value) => updateTransitionLeg(index, { weight: Math.max(0.01, value) })} />
                        <button aria-label={t("removeLeg")} className="mb-2 h-9 text-white/35 hover:text-red-300 disabled:opacity-20" disabled={(spec.transitionLegs?.length ?? 0) <= 1} onClick={() => removeTransitionLeg(index)} type="button"><Trash2 className="mx-auto h-3.5 w-3.5" /></button>
                      </div>
                    ))}
                    <Button className="w-full rounded-none" disabled={(spec.transitionLegs?.length ?? 0) >= 4} size="sm" variant="outline" onClick={addTransitionLeg}><Plus className="h-3.5 w-3.5" />{t("addLeg")}</Button>
                  </div>
                )}
                <p className="instrument-note">{t("transitionHint")}</p>
              </>
            )}
          </Panel>

          <Panel index="03" title={t("sections.coloring")}>
            <Control label={t("colorMap")}>
              <select className={selectClass} disabled={Boolean(spec.colorProgram)} value={spec.colorMap ?? ""} onChange={(event) => update({ colorMap: event.target.value })}>
                {capabilities.colorMaps.map((item) => <option key={item} value={item}>{t(`colorMaps.${item}.name`)}</option>)}
              </select>
            </Control>
            {!spec.colorProgram && selectedColor && (
              <div className="mb-3 border border-white/10 bg-black/20 p-2">
                <div className="h-4" style={{ background: selectedColor.preview }} />
                <p className="mt-2 text-xs leading-relaxed text-white/60">{t(`colorMaps.${selectedColor.id}.description`)}</p>
                <p className="mt-1 text-[11px] text-white/35"><span className="text-amber-200/60">{t("bestFor")}</span> {t(`colorMaps.${selectedColor.id}.bestFor`)}</p>
                <p className="mt-1 text-[11px] text-white/35"><span className="text-amber-200/60">{t("cost")}</span> {t(`colorMaps.${selectedColor.id}.cost`)}</p>
              </div>
            )}
            <div className="grid grid-cols-2 gap-2">
              <Control label={t("colorMode")}>
                <select className={selectClass} disabled={Boolean(spec.colorProgram)} value={spec.colorMode ?? "direct"} onChange={(event) => update({ colorMode: event.target.value as NonNullable<FractalSpec["colorMode"]> })}>
                  {capabilities.colorModes.map((item) => <option key={item} value={item}>{t(`colorModes.${item}.name`)}</option>)}
                </select>
              </Control>
              <NumberControl label={t("cyclesPerOctave")} disabled={Boolean(spec.colorProgram)} min="0.01" max="64" step="0.1" value={spec.cyclesPerOctave ?? 1} onChange={(value) => update({ cyclesPerOctave: value })} />
            </div>
            <p className="instrument-note">{t(`colorModes.${spec.colorMode ?? "direct"}.description`)}</p>
            {spec.metric !== "escape" && <label className="instrument-check mt-3"><span>{t("smooth")}</span><input checked={Boolean(spec.smooth)} onChange={(event) => update({ smooth: event.target.checked })} type="checkbox" /></label>}
            <label className="instrument-check mt-2"><span>{t("customGradient")}</span><input checked={Boolean(spec.colorProgram)} disabled={!capabilities.customGradient.enabled} onChange={(event) => toggleGradient(event.target.checked)} type="checkbox" /></label>
            {spec.colorProgram && (
              <GradientEditor
                maxStops={capabilities.customGradient.maxStops}
                onAdd={addGradientStop}
                onChange={updateGradient}
                onRemove={removeGradientStop}
                onStopChange={updateGradientStop}
                program={spec.colorProgram}
                t={t}
              />
            )}
          </Panel>

          <details className="instrument-panel group">
            <summary className="cursor-pointer list-none px-3 py-2 text-xs uppercase tracking-[0.14em] text-white/55">04 · {t("sections.compute")}</summary>
            <div className="border-t border-white/10 p-3">
              <div className="grid grid-cols-2 gap-2">
                <NumberControl label={t("iterations")} min="1" max="1000000" step="16" value={spec.iterations ?? 512} onChange={(value) => update({ iterations: Math.round(value) })} />
                <NumberControl label={t("bailout")} min="0.01" step="0.5" value={spec.bailout ?? 2} onChange={(value) => update({ bailout: value })} />
                <NumberControl label={t("rotation")} min="-360" max="360" step="1" value={spec.rotationDeg ?? 0} onChange={(value) => update({ rotationDeg: value })} />
                <Control label={t("metric")}>
                  <select className={selectClass} disabled={mode === "formula" || mode === "sequence"} value={spec.metric} onChange={(event) => update({ metric: event.target.value as NonNullable<FractalSpec["metric"]> })}>{activeMetrics.map((item) => <option key={item} value={item}>{t(`metrics.${item}.name`)}</option>)}</select>
                </Control>
                {spec.metric === "min_pairwise_dist" && <NumberControl label={t("pairwiseCap")} min="1" max="1000000" step="1" value={spec.pairwiseCap ?? 64} onChange={(value) => update({ pairwiseCap: Math.round(value) })} />}
                <Control label={t("engine")}>
                  <select className={selectClass} value={spec.engine} onChange={(event) => update({ engine: event.target.value as NonNullable<FractalSpec["engine"]> })}>{capabilities.engines.map((item) => <option key={item} value={item}>{item}</option>)}</select>
                </Control>
                <Control label={t("scalar")}>
                  <select className={selectClass} value={spec.scalarType} onChange={(event) => update({ scalarType: event.target.value as NonNullable<FractalSpec["scalarType"]> })}>{capabilities.scalars.map((item) => <option key={item} value={item}>{item}</option>)}</select>
                </Control>
              </div>
              <p className="instrument-note">{t(`metrics.${spec.metric ?? "escape"}.description`)}</p>
            </div>
          </details>
        </aside>

        <main className="min-w-0 space-y-3">
          <section className="instrument-panel p-3">
            <div className="mb-2 flex flex-wrap items-center justify-between gap-2">
              <div className="flex items-center gap-2">
                <span className="instrument-kicker">{t("viewport")}</span>
                <span className="font-mono text-[11px] text-white/35">{t(modeLabelKey(mode))} · {zoomLevel.toFixed(2)} oct</span>
              </div>
              <span className="font-mono text-[10px] text-white/30">{t("centerAlwaysVisible")}</span>
            </div>
            {mode === "julia" ? (
              <div className="grid gap-3 lg:grid-cols-2">
                <div className="border-l border-amber-300/35 pl-3">
                  <p className="mb-2 font-mono text-[10px] uppercase tracking-[0.15em] text-amber-200/65">A · {t("juliaParameterPlane")}</p>
                  <div className="grid gap-2 sm:grid-cols-2 xl:grid-cols-[minmax(0,1fr)_minmax(0,1fr)_8rem]">
                    <PreciseControl label={t("centerRe")} value={juliaPickerSpec.centerReStr ?? String(juliaPickerSpec.centerRe ?? 0)} onCommit={(value) => setJuliaPickerSpec((current) => ({ ...current, centerReStr: value, centerRe: Number(value) }))} />
                    <PreciseControl label={t("centerIm")} value={juliaPickerSpec.centerImStr ?? String(juliaPickerSpec.centerIm ?? 0)} onCommit={(value) => setJuliaPickerSpec((current) => ({ ...current, centerImStr: value, centerIm: Number(value) }))} />
                    <NumberControl label={t("scale")} min={String(minScale)} step="0.000001" value={juliaPickerSpec.scale ?? 3} onChange={(value) => setJuliaPickerSpec((current) => ({ ...current, scale: Math.max(minScale, value) }))} />
                  </div>
                </div>
                <div className="border-l border-white/15 pl-3">
                  <p className="mb-2 font-mono text-[10px] uppercase tracking-[0.15em] text-white/50">B · {t("juliaDynamicalPlane")}</p>
                  <div className="grid gap-2 sm:grid-cols-2 xl:grid-cols-[minmax(0,1fr)_minmax(0,1fr)_8rem]">
                    <PreciseControl label={t("centerRe")} value={spec.centerReStr ?? String(spec.centerRe ?? 0)} onCommit={(value) => update({ centerReStr: value, centerRe: Number(value) })} />
                    <PreciseControl label={t("centerIm")} value={spec.centerImStr ?? String(spec.centerIm ?? 0)} onCommit={(value) => update({ centerImStr: value, centerIm: Number(value) })} />
                    <NumberControl label={t("scale")} min={String(minScale)} step="0.000001" value={spec.scale ?? 3} onChange={(value) => update({ scale: Math.max(minScale, value) })} />
                  </div>
                </div>
              </div>
            ) : (
              <div className="grid gap-2 sm:grid-cols-2 lg:grid-cols-[minmax(0,1fr)_minmax(0,1fr)_10rem]">
                <PreciseControl label={t("centerRe")} value={spec.centerReStr ?? String(spec.centerRe ?? 0)} onCommit={(value) => update({ centerReStr: value, centerRe: Number(value) })} />
                <PreciseControl label={t("centerIm")} value={spec.centerImStr ?? String(spec.centerIm ?? 0)} onCommit={(value) => update({ centerImStr: value, centerIm: Number(value) })} />
                <NumberControl label={t("scale")} min={String(minScale)} step="0.000001" value={spec.scale ?? 3} onChange={(value) => update({ scale: Math.max(minScale, value) })} />
              </div>
            )}
          </section>

          {mode === "julia" ? (
            <section className="grid gap-3 lg:grid-cols-2">
              <div className="instrument-panel overflow-hidden p-2">
                <div className="mb-2 flex items-start justify-between gap-3 px-1">
                  <div><p className="instrument-kicker">A · {t("juliaParameterPlane")}</p><p className="mt-1 text-xs text-white/40">{t("juliaPickerHint")}</p></div>
                  <span className="shrink-0 font-mono text-[10px] text-amber-200/70">c = {formatCoordinate(spec.juliaRe)} {Number(spec.juliaIm ?? 0) < 0 ? "−" : "+"} {formatCoordinate(Math.abs(Number(spec.juliaIm ?? 0)))}i</span>
                </div>
                <InteractiveFractalCanvas
                  exportHeight={previewSize.height}
                  exportWidth={previewSize.width}
                  height={previewSize.height}
                  labels={{ alt: t("canvas.parameterAlt"), empty: t("canvas.empty"), hint: t("canvas.selectHint"), detail: t("canvas.detail"), rendering: t("canvas.rendering"), zoomOut: t("canvas.zoomOut"), zoomIn: t("canvas.zoomIn"), reset: t("resetParameter"), frame: t("canvas.parameterFrame") }}
                  onChange={updateJuliaPickerViewport}
                  onNavigationStart={() => undefined}
                  onPointSelect={({ re, im }) => update({ juliaRe: re, juliaIm: im })}
                  onReset={resetJuliaPickerViewport}
                  onZoom={zoomJuliaPicker}
                  preview={juliaPickerPreview}
                  previewing={previewing}
                  selection={{ re: Number(spec.juliaRe ?? -0.8), im: Number(spec.juliaIm ?? 0.156) }}
                  spec={juliaPickerCanonical}
                  width={previewSize.width}
                />
              </div>
              <div className="instrument-panel overflow-hidden p-2">
                <div className="mb-2 px-1"><p className="instrument-kicker">B · {t("juliaDynamicalPlane")}</p><p className="mt-1 text-xs text-white/40">{t("juliaOutputHint")}</p></div>
                <InteractiveFractalCanvas
                  exportHeight={output.height}
                  exportWidth={output.width}
                  height={previewSize.height}
                  labels={{ alt: t("canvas.juliaAlt"), empty: t("canvas.empty"), hint: t("canvas.hint"), detail: t("canvas.detail"), rendering: t("canvas.rendering"), zoomOut: t("canvas.zoomOut"), zoomIn: t("canvas.zoomIn"), reset: t("resetJulia"), frame: t("canvas.frame") }}
                  onChange={updateViewport}
                  onNavigationStart={() => undefined}
                  onReset={resetJuliaViewport}
                  onZoom={zoom}
                  preview={preview}
                  previewing={previewing}
                  spec={canonical}
                  width={previewSize.width}
                />
              </div>
            </section>
          ) : (
            <InteractiveFractalCanvas
              exportHeight={output.height}
              exportWidth={output.width}
              height={previewSize.height}
              labels={{ alt: t("canvas.alt"), empty: t("canvas.empty"), hint: t("canvas.hint"), detail: t("canvas.detail"), rendering: t("canvas.rendering"), zoomOut: t("canvas.zoomOut"), zoomIn: t("canvas.zoomIn"), reset: t("reset"), frame: t("canvas.frame") }}
              onChange={updateViewport}
              onNavigationStart={() => undefined}
              onReset={reset}
              onZoom={zoom}
              preview={preview}
              previewing={previewing}
              spec={canonical}
              width={previewSize.width}
            />
          )}

          {error && <p className="border border-red-500/30 bg-red-500/10 px-3 py-2 text-sm text-red-200">{error}</p>}
          {notice && <p className="border border-amber-400/30 bg-amber-400/10 px-3 py-2 text-sm text-amber-100">{notice}</p>}

          <section className="instrument-panel p-3">
            <div className="mb-3 flex items-center gap-2">
              <ImageDown className="h-4 w-4 text-amber-300/80" />
              <h2 className="text-sm font-medium text-white/80">{t("sections.export")}</h2>
              <span className="ml-auto font-mono text-[10px] uppercase tracking-wider text-white/30">PNG · sRGB</span>
            </div>
            <div className="grid gap-2 md:grid-cols-[minmax(12rem,1fr)_8rem_8rem_auto] md:items-end">
              <Control label={t("outputPreset")}>
                <select className={selectClass} value={output.preset} onChange={(event) => chooseOutputPreset(event.target.value)}>
                  {OUTPUT_PRESETS.map((item) => <option key={item.id} value={item.id}>{t(`outputPresets.${item.id}`)} · {item.width}×{item.height}</option>)}
                  <option value="custom">{t("outputPresets.custom")}</option>
                </select>
              </Control>
              <ClampedNumberControl label={t("width")} min={64} max={4096} step="1" value={output.width} onCommit={(value) => updateOutputDimension("width", value)} />
              <ClampedNumberControl label={t("height")} min={64} max={4096} step="1" value={output.height} onCommit={(value) => updateOutputDimension("height", value)} />
              <Button className="h-9 rounded-none border-amber-300/30 bg-amber-500/15 text-amber-100 hover:bg-amber-500/25" disabled={exporting} onClick={() => void saveAndRender()}>
                <Save className="h-4 w-4" />{exporting ? t("creatingExport") : t("exportPng")}
              </Button>
            </div>
            <p className="mt-2 text-xs text-white/35">{t("compositionHint")}</p>

            {job && (
              <div className="mt-3 border-t border-white/10 pt-3">
                <div className="flex flex-wrap items-center gap-3 text-sm">
                  {job.status === "completed" ? <CheckCircle2 className="h-4 w-4 text-emerald-400" /> : <span className="h-2 w-2 bg-amber-400" />}
                  <span>{t("exportJob")}: <b className="font-mono font-normal text-white/80">{job.status}</b></span>
                  <span className="font-mono text-white/45">{job.progressPercent}%</span>
                  <div className="h-1.5 min-w-28 flex-1 overflow-hidden bg-white/10"><div className="h-full bg-amber-400 transition-[width]" style={{ width: `${job.progressPercent}%` }} /></div>
                  {!terminalStatuses.has(job.status) && <Button className="rounded-none" size="sm" variant="outline" onClick={() => void cancelJob()}>{t("cancelExport")}</Button>}
                  {job.status === "completed" && job.assetId && (
                    <>
                      <Button className="rounded-none" size="sm" onClick={() => void downloadAsset()}><Download className="h-3.5 w-3.5" />{t("downloadPng")}</Button>
                      <Button asChild className="rounded-none" size="sm" variant="outline"><Link href="/assets"><ExternalLink className="h-3.5 w-3.5" />{t("openLibrary")}</Link></Button>
                    </>
                  )}
                </div>
                {job.errorCode && <p className="mt-2 font-mono text-xs text-red-300">{job.errorCode}</p>}
              </div>
            )}
          </section>
        </main>
      </div>
    </div>
  );
}

function Panel({ index, title, children }: { index: string; title: string; children: React.ReactNode }) {
  return (
    <section className="instrument-panel">
      <h2 className="border-b border-white/10 px-3 py-2 text-xs uppercase tracking-[0.14em] text-white/55"><span className="mr-2 font-mono text-amber-300/65">{index}</span>{title}</h2>
      <div className="p-3">{children}</div>
    </section>
  );
}

function Control({ label, children }: { label: string; children: React.ReactNode }) {
  return <label className="mb-2 block text-[11px] text-white/45"><span className="mb-1 block uppercase tracking-wider">{label}</span>{children}</label>;
}

function NumberControl({ label, value, onChange, ...props }: { label: string; value: number; onChange: (value: number) => void } & Omit<React.ComponentProps<typeof Input>, "value" | "onChange">) {
  return <Control label={label}><Input className="instrument-control h-9 rounded-none font-mono text-xs" value={Number.isFinite(value) ? value : 0} type="number" onChange={(event) => onChange(Number(event.target.value))} {...props} /></Control>;
}

function ClampedNumberControl({ label, value, min, max, onCommit, ...props }: { label: string; value: number; min: number; max: number; onCommit: (value: number) => void } & Omit<React.ComponentProps<typeof Input>, "value" | "onChange" | "min" | "max">) {
  const [draft, setDraft] = useState(String(value));
  useEffect(() => setDraft(String(value)), [value]);
  const commit = () => {
    const trimmed = draft.trim();
    const parsed = Number(trimmed);
    if (!trimmed || !Number.isFinite(parsed)) {
      setDraft(String(value));
      return;
    }
    const clamped = Math.max(min, Math.min(max, Math.round(parsed)));
    setDraft(String(clamped));
    onCommit(clamped);
  };
  return (
    <Control label={label}>
      <Input
        className="instrument-control h-9 rounded-none font-mono text-xs"
        max={max}
        min={min}
        type="number"
        value={draft}
        onBlur={commit}
        onChange={(event) => setDraft(event.target.value)}
        onKeyDown={(event) => { if (event.key === "Enter") event.currentTarget.blur(); }}
        {...props}
      />
    </Control>
  );
}

function PreciseControl({ label, value, onCommit }: { label: string; value: string; onCommit: (value: string) => void }) {
  const [draft, setDraft] = useState(value);
  useEffect(() => setDraft(value), [value]);
  const commit = () => {
    const trimmed = draft.trim();
    if (trimmed && Number.isFinite(Number(trimmed))) onCommit(trimmed);
    else setDraft(value);
  };
  return (
    <Control label={label}>
      <Input className="instrument-control h-9 rounded-none font-mono text-xs" inputMode="decimal" value={draft} onBlur={commit} onChange={(event) => setDraft(event.target.value)} onKeyDown={(event) => { if (event.key === "Enter") event.currentTarget.blur(); }} />
    </Control>
  );
}

function GradientEditor({ program, maxStops, onChange, onStopChange, onAdd, onRemove, t }: {
  program: NonNullable<FractalSpec["colorProgram"]>;
  maxStops: number;
  onChange: (patch: Partial<NonNullable<FractalSpec["colorProgram"]>>) => void;
  onStopChange: (index: number, patch: Partial<{ at: number; color: string }>) => void;
  onAdd: () => void;
  onRemove: (index: number) => void;
  t: ReturnType<typeof useTranslations<"studio">>;
}) {
  return (
    <div className="mt-3 border-t border-white/10 pt-3">
      <div className="mb-3 h-5 border border-white/15" style={{ background: `linear-gradient(90deg, ${program.stops.map((stop) => `${stop.color} ${stop.at * 100}%`).join(", ")})` }} />
      <div className="space-y-1.5">
        {program.stops.map((stop, index) => (
          <div className="grid grid-cols-[2.5rem_minmax(0,1fr)_2rem_1.5rem] items-center gap-1.5" key={index}>
            <input aria-label={t("gradientColor")} className="h-7 w-10 bg-transparent" type="color" value={stop.color} onChange={(event) => onStopChange(index, { color: event.target.value })} />
            <input
              aria-label={t("gradientPosition")}
              className="accent-amber-500 disabled:opacity-30"
              disabled={index === 0 || index === program.stops.length - 1}
              max={index === program.stops.length - 1 ? 1 : program.stops[index + 1]!.at - 0.001}
              min={index === 0 ? 0 : program.stops[index - 1]!.at + 0.001}
              step="0.001"
              type="range"
              value={stop.at}
              onChange={(event) => onStopChange(index, { at: Number(event.target.value) })}
            />
            <span className="font-mono text-[10px] text-white/45">{Math.round(stop.at * 100)}</span>
            <button aria-label={t("removeColor")} className="text-white/30 hover:text-red-300 disabled:opacity-20" disabled={program.stops.length <= 2} onClick={() => onRemove(index)} type="button"><Trash2 className="h-3.5 w-3.5" /></button>
          </div>
        ))}
      </div>
      <Button className="mt-2 w-full rounded-none" disabled={program.stops.length >= maxStops} size="sm" variant="outline" onClick={onAdd}><Plus className="h-3.5 w-3.5" />{t("addColor")}</Button>
      <div className="mt-3 grid grid-cols-2 gap-2">
        <NumberControl label={t("gradientCycles")} min="0.01" max="64" step="0.1" value={program.cycles ?? 1} onChange={(value) => onChange({ cycles: value })} />
        <NumberControl label={t("gradientPhase")} min="-1000" max="1000" step="0.05" value={program.phase ?? 0} onChange={(value) => onChange({ phase: value })} />
        <Control label={t("gradientWrap")}>
          <select className={selectClass} value={program.wrap ?? "repeat"} onChange={(event) => onChange({ wrap: event.target.value as "clamp" | "repeat" | "mirror" })}><option value="repeat">{t("wrap.repeat")}</option><option value="clamp">{t("wrap.clamp")}</option><option value="mirror">{t("wrap.mirror")}</option></select>
        </Control>
        <div className="grid grid-cols-2 gap-2">
          <Control label={t("interiorColor")}><input aria-label={t("interiorColor")} className="h-9 w-full bg-transparent" type="color" value={program.interiorColor ?? "#050505"} onChange={(event) => onChange({ interiorColor: event.target.value })} /></Control>
          <Control label={t("invalidColor")}><input aria-label={t("invalidColor")} className="h-9 w-full bg-transparent" type="color" value={program.invalidColor ?? "#ff00ff"} onChange={(event) => onChange({ invalidColor: event.target.value })} /></Control>
        </div>
      </div>
      <p className="instrument-note">{t("customGradientHint")}</p>
    </div>
  );
}
