"use client";

import { useTranslations } from "next-intl";
import { useStudioStore } from "@/stores/studio-store";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "@/components/ui/select";
import { Card, CardContent, CardHeader, CardTitle } from "@/components/ui/card";
import { VARIANTS, METRICS, COLORMAPS } from "@/types/api";
import { RefreshCw } from "lucide-react";

interface ParamPanelProps {
  onRender: () => void;
  isRendering: boolean;
}

export function ParamPanel({ onRender, isRendering }: ParamPanelProps) {
  const t = useTranslations("studio");
  const store = useStudioStore();

  return (
    <div className="w-72 flex-shrink-0 space-y-4 overflow-y-auto">
      <Card>
        <CardHeader className="pb-2">
          <CardTitle className="text-sm font-medium">{t("parameters")}</CardTitle>
        </CardHeader>
        <CardContent className="space-y-3">
          {/* Center */}
          <div className="grid grid-cols-2 gap-2">
            <div>
              <label className="text-xs text-muted-foreground">Re</label>
              <Input
                type="number"
                value={store.centerRe}
                onChange={(e) => store.setCenter(Number(e.target.value), store.centerIm)}
                step={0.0001}
                className="h-8 text-xs"
              />
            </div>
            <div>
              <label className="text-xs text-muted-foreground">Im</label>
              <Input
                type="number"
                value={store.centerIm}
                onChange={(e) => store.setCenter(store.centerRe, Number(e.target.value))}
                step={0.0001}
                className="h-8 text-xs"
              />
            </div>
          </div>

          {/* Scale */}
          <div>
            <label className="text-xs text-muted-foreground">{t("scale")}</label>
            <Input
              type="number"
              value={store.scale}
              onChange={(e) => store.setScale(Number(e.target.value))}
              step={0.1}
              min={0.000001}
              className="h-8 text-xs"
            />
          </div>

          {/* Resolution */}
          <div className="grid grid-cols-2 gap-2">
            <div>
              <label className="text-xs text-muted-foreground">{t("width")}</label>
              <Input
                type="number"
                value={store.width}
                onChange={(e) => store.setResolution(Number(e.target.value), store.height)}
                min={64}
                max={16384}
                className="h-8 text-xs"
              />
            </div>
            <div>
              <label className="text-xs text-muted-foreground">{t("height")}</label>
              <Input
                type="number"
                value={store.height}
                onChange={(e) => store.setResolution(store.width, Number(e.target.value))}
                min={64}
                max={16384}
                className="h-8 text-xs"
              />
            </div>
          </div>

          {/* Iterations */}
          <div>
            <label className="text-xs text-muted-foreground">{t("iterations")}</label>
            <Input
              type="number"
              value={store.iterations}
              onChange={(e) => store.setIterations(Number(e.target.value))}
              min={1}
              max={1000000}
              className="h-8 text-xs"
            />
          </div>

          {/* Rotation */}
          <div>
            <label className="text-xs text-muted-foreground">{t("rotation")} (°)</label>
            <Input
              type="number"
              value={store.rotationDeg}
              onChange={(e) => store.setRotation(Number(e.target.value))}
              step={1}
              className="h-8 text-xs"
            />
          </div>

          {/* Variant */}
          <div>
            <label className="text-xs text-muted-foreground">{t("variant")}</label>
            <Select value={store.variant} onValueChange={store.setVariant}>
              <SelectTrigger className="h-8 text-xs">
                <SelectValue />
              </SelectTrigger>
              <SelectContent>
                {VARIANTS.map((v) => (
                  <SelectItem key={v} value={v} className="text-xs">
                    {v}
                  </SelectItem>
                ))}
              </SelectContent>
            </Select>
          </div>

          {/* Metric */}
          <div>
            <label className="text-xs text-muted-foreground">{t("metric")}</label>
            <Select value={store.metric} onValueChange={store.setMetric}>
              <SelectTrigger className="h-8 text-xs">
                <SelectValue />
              </SelectTrigger>
              <SelectContent>
                {METRICS.map((m) => (
                  <SelectItem key={m} value={m} className="text-xs">
                    {m}
                  </SelectItem>
                ))}
              </SelectContent>
            </Select>
          </div>

          {/* Color Map */}
          <div>
            <label className="text-xs text-muted-foreground">{t("colorMap")}</label>
            <Select value={store.colorMap} onValueChange={store.setColorMap}>
              <SelectTrigger className="h-8 text-xs">
                <SelectValue />
              </SelectTrigger>
              <SelectContent>
                {COLORMAPS.map((c) => (
                  <SelectItem key={c} value={c} className="text-xs">
                    {c}
                  </SelectItem>
                ))}
              </SelectContent>
            </Select>
          </div>

          {/* Julia Toggle */}
          <div className="flex items-center gap-2">
            <input
              type="checkbox"
              id="julia-toggle"
              checked={store.julia}
              onChange={(e) => store.setJulia(e.target.checked)}
              className="h-4 w-4 rounded border-gray-300"
            />
            <label htmlFor="julia-toggle" className="text-xs text-muted-foreground">
              {t("julia")}
            </label>
          </div>

          {store.julia && (
            <div className="grid grid-cols-2 gap-2">
              <div>
                <label className="text-xs text-muted-foreground">Julia Re</label>
                <Input
                  type="number"
                  value={store.juliaRe}
                  onChange={(e) => store.setJuliaParams(Number(e.target.value), store.juliaIm)}
                  step={0.01}
                  className="h-8 text-xs"
                />
              </div>
              <div>
                <label className="text-xs text-muted-foreground">Julia Im</label>
                <Input
                  type="number"
                  value={store.juliaIm}
                  onChange={(e) => store.setJuliaParams(store.juliaRe, Number(e.target.value))}
                  step={0.01}
                  className="h-8 text-xs"
                />
              </div>
            </div>
          )}

          {/* Smooth */}
          <div className="flex items-center gap-2">
            <input
              type="checkbox"
              id="smooth-toggle"
              checked={store.smooth}
              onChange={(e) => store.setSmooth(e.target.checked)}
              className="h-4 w-4 rounded border-gray-300"
            />
            <label htmlFor="smooth-toggle" className="text-xs text-muted-foreground">
              {t("smooth")}
            </label>
          </div>

          {/* Engine */}
          <div>
            <label className="text-xs text-muted-foreground">{t("engine")}</label>
            <Select value={store.engine} onValueChange={store.setEngine}>
              <SelectTrigger className="h-8 text-xs">
                <SelectValue />
              </SelectTrigger>
              <SelectContent>
                {["auto", "openmp", "avx2", "avx512", "cuda", "hybrid"].map((e) => (
                  <SelectItem key={e} value={e} className="text-xs">{e}</SelectItem>
                ))}
              </SelectContent>
            </Select>
          </div>

          {/* Scalar */}
          <div>
            <label className="text-xs text-muted-foreground">{t("scalar")}</label>
            <Select value={store.scalarType} onValueChange={store.setScalarType}>
              <SelectTrigger className="h-8 text-xs">
                <SelectValue />
              </SelectTrigger>
              <SelectContent>
                {["auto", "fp32", "fp64", "fx64", "fp80", "fp128"].map((s) => (
                  <SelectItem key={s} value={s} className="text-xs">{s}</SelectItem>
                ))}
              </SelectContent>
            </Select>
          </div>
        </CardContent>
      </Card>

      {/* Render button */}
      <Button
        variant="fractal"
        className="w-full"
        onClick={onRender}
        disabled={isRendering}
      >
        <RefreshCw className={`h-4 w-4 ${isRendering ? "animate-spin" : ""}`} />
        {isRendering ? t("rendering") : t("render")}
      </Button>
    </div>
  );
}
