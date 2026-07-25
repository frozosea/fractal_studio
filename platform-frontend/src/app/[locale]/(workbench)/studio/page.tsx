"use client";

import { useState, useCallback } from "react";
import { FractalCanvas } from "@/components/studio/fractal-canvas";
import { ParamPanel } from "@/components/studio/param-panel";
import { useStudioStore } from "@/stores/studio-store";
import { useMapRenderInline } from "@/lib/hooks/use-studio";
import type { MapRenderRequest } from "@/types/map";

export default function StudioPage() {
  const store = useStudioStore();
  const renderMutation = useMapRenderInline();
  const [imageData, setImageData] = useState<ArrayBuffer | null>(null);
  const [renderMs, setRenderMs] = useState(0);
  const [error, setError] = useState<string | null>(null);
  const [abortController, setAbortController] =
    useState<AbortController | null>(null);

  const doRender = useCallback(async () => {
    if (abortController) {
      abortController.abort();
    }
    const controller = new AbortController();
    setAbortController(controller);
    setError(null);

    const req: MapRenderRequest = {
      centerRe: store.centerRe,
      centerIm: store.centerIm,
      scale: store.scale,
      width: store.width,
      height: store.height,
      iterations: store.iterations,
      variant: store.variant,
      metric: store.metric as MapRenderRequest["metric"],
      colorMap: store.colorMap as MapRenderRequest["colorMap"],
      smooth: store.smooth,
      julia: store.julia,
      juliaRe: store.juliaRe,
      juliaIm: store.juliaIm,
      engine: store.engine === "auto" ? undefined : store.engine,
      scalarType: store.scalarType === "auto" ? undefined : store.scalarType,
      rotationDeg: store.rotationDeg || undefined,
    };

    try {
      const result = await renderMutation.mutateAsync({
        req,
        signal: controller.signal,
      });
      if (result.data) {
        setImageData(result.data);
        setRenderMs(result.generatedMs);
      }
    } catch (err: unknown) {
      if (err instanceof DOMException && err.name === "AbortError") return;
      setError(err instanceof Error ? err.message : "Render failed");
    }
  }, [store, renderMutation, abortController]);

  return (
    <div className="flex h-full gap-6">
      {/* Main canvas area */}
      <div className="flex-1">
        <FractalCanvas
          imageData={imageData}
          width={store.width}
          height={store.height}
          error={error}
          renderMs={renderMs}
          onRender={doRender}
          isRendering={renderMutation.isPending}
        />
      </div>

      {/* Parameter panel */}
      <ParamPanel onRender={doRender} isRendering={renderMutation.isPending} />
    </div>
  );
}
