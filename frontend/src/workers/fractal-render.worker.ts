/// <reference lib="webworker" />

import { renderLocalRgba, type LocalRenderSpec } from "@/lib/fractal/local-render-core";

type RenderMessage = { id: number; spec: LocalRenderSpec; width: number; height: number };

self.onmessage = async (event: MessageEvent<RenderMessage>) => {
  const { id, spec, width, height } = event.data;
  try {
    const rgba = renderLocalRgba(spec, width, height);
    const canvas = new OffscreenCanvas(width, height);
    const context = canvas.getContext("2d", { alpha: false });
    if (!context) throw new Error("2d_context_unavailable");
    context.putImageData(new ImageData(rgba, width, height), 0, 0);
    const blob = await canvas.convertToBlob({ type: "image/png" });
    self.postMessage({ id, blob });
  } catch (reason) {
    self.postMessage({ id, error: reason instanceof Error ? reason.message : "local_render_failed" });
  }
};

export {};
