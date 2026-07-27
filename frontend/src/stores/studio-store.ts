import { create } from 'zustand';

export interface StudioState {
  // Viewport state
  centerRe: number;
  centerIm: number;
  scale: number;
  width: number;
  height: number;
  iterations: number;
  variant: string;
  metric: string;
  colorMap: string;
  julia: boolean;
  juliaRe: number;
  juliaIm: number;
  smooth: boolean;
  engine: string;
  scalarType: string;
  rotationDeg: number;

  // Actions
  setCenter: (re: number, im: number) => void;
  setScale: (scale: number) => void;
  setWidth: (width: number) => void;
  setHeight: (height: number) => void;
  setIterations: (iterations: number) => void;
  setVariant: (variant: string) => void;
  setMetric: (metric: string) => void;
  setColorMap: (colorMap: string) => void;
  setJulia: (julia: boolean) => void;
  setJuliaRe: (re: number) => void;
  setJuliaIm: (im: number) => void;
  setSmooth: (smooth: boolean) => void;
  setEngine: (engine: string) => void;
  setScalarType: (scalarType: string) => void;
  setRotationDeg: (rotationDeg: number) => void;
  setResolution: (width: number, height: number) => void;
  setRotation: (deg: number) => void;
  setJuliaParams: (re: number, im: number) => void;
  reset: () => void;
}

const DEFAULT_CENTER_RE = -0.75;
const DEFAULT_CENTER_IM = 0;
const DEFAULT_SCALE = 3.0;
const DEFAULT_WIDTH = 1024;
const DEFAULT_HEIGHT = 768;
const DEFAULT_ITERATIONS = 1024;
const DEFAULT_VARIANT = 'mandelbrot';
const DEFAULT_METRIC = 'escape';
const DEFAULT_COLOR_MAP = 'classic_cos';
const DEFAULT_ENGINE = 'auto';
const DEFAULT_SCALAR_TYPE = 'auto';

const initialState = {
  centerRe: DEFAULT_CENTER_RE,
  centerIm: DEFAULT_CENTER_IM,
  scale: DEFAULT_SCALE,
  width: DEFAULT_WIDTH,
  height: DEFAULT_HEIGHT,
  iterations: DEFAULT_ITERATIONS,
  variant: DEFAULT_VARIANT,
  metric: DEFAULT_METRIC,
  colorMap: DEFAULT_COLOR_MAP,
  julia: false,
  juliaRe: 0,
  juliaIm: 0,
  smooth: true,
  engine: DEFAULT_ENGINE,
  scalarType: DEFAULT_SCALAR_TYPE,
  rotationDeg: 0,
};

export const useStudioStore = create<StudioState>()((set) => ({
  ...initialState,

  setCenter: (re: number, im: number) => set({ centerRe: re, centerIm: im }),
  setScale: (scale: number) => set({ scale }),
  setWidth: (width: number) => set({ width }),
  setHeight: (height: number) => set({ height }),
  setIterations: (iterations: number) => set({ iterations }),
  setVariant: (variant: string) => set({ variant }),
  setMetric: (metric: string) => set({ metric }),
  setColorMap: (colorMap: string) => set({ colorMap }),
  setJulia: (julia: boolean) => set({ julia }),
  setJuliaRe: (re: number) => set({ juliaRe: re }),
  setJuliaIm: (im: number) => set({ juliaIm: im }),
  setSmooth: (smooth: boolean) => set({ smooth }),
  setEngine: (engine: string) => set({ engine }),
  setScalarType: (scalarType: string) => set({ scalarType }),
  setRotationDeg: (rotationDeg: number) => set({ rotationDeg }),
  setResolution: (width: number, height: number) => set({ width, height }),
  setRotation: (rotationDeg: number) => set({ rotationDeg }),
  setJuliaParams: (juliaRe: number, juliaIm: number) => set({ juliaRe, juliaIm }),
  reset: () => set(initialState),
}));
