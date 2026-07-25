"use client";

import { useEffect, useRef } from "react";
import { useTranslations } from "next-intl";
import { Button } from "@/components/ui/button";
import { LoadingSpinner } from "@/components/shared/loading-spinner";
import { RefreshCw, ZoomIn } from "lucide-react";

interface FractalCanvasProps {
  imageData: ArrayBuffer | null;
  width: number;
  height: number;
  error: string | null;
  renderMs: number;
  onRender: () => void;
  isRendering: boolean;
}

export function FractalCanvas({
  imageData,
  width,
  height,
  error,
  renderMs,
  onRender,
  isRendering,
}: FractalCanvasProps) {
  const t = useTranslations("studio");
  const canvasRef = useRef<HTMLCanvasElement>(null);

  useEffect(() => {
    if (!imageData || !canvasRef.current) return;
    const canvas = canvasRef.current;
    const ctx = canvas.getContext("2d");
    if (!ctx) return;

    const blob = new Blob([imageData], { type: "application/octet-stream" });
    const url = URL.createObjectURL(blob);
    const img = new Image();
    img.onload = () => {
      canvas.width = img.width;
      canvas.height = img.height;
      ctx.drawImage(img, 0, 0);
      URL.revokeObjectURL(url);
    };
    img.src = url;
  }, [imageData]);

  const displayW = Math.min(width, 1024);
  const displayH = Math.min(height, 768);

  return (
    <div className="flex h-full flex-col">
      {/* Toolbar — whisper */}
      <div className="mb-4 flex items-center justify-between">
        <div className="flex items-center gap-2">
          <Button
            variant="fractal"
            size="sm"
            onClick={onRender}
            disabled={isRendering}
          >
            {isRendering ? (
              <>
                <LoadingSpinner className="h-4 w-4" />
                {t("rendering")}
              </>
            ) : (
              <>
                <RefreshCw className="h-4 w-4" />
                {t("render")}
              </>
            )}
          </Button>
          {renderMs > 0 && (
            <span className="text-xs text-white/35">
              {renderMs.toFixed(0)}ms
            </span>
          )}
        </div>
        <div className="text-xs text-white/30">
          {width}×{height}
        </div>
      </div>

      {/* Canvas — deep void with soft glow */}
      <div
        className="flex-1 overflow-hidden rounded-xl canvas-glow"
        style={{
          background:
            "radial-gradient(ellipse 80% 60% at 50% 50%, hsl(228 45% 10%) 0%, hsl(228 50% 5%) 100%)",
          border: "1px solid hsl(226 22% 16% / 0.4)",
        }}
      >
        {error ? (
          <div className="flex h-full flex-col items-center justify-center gap-4 p-8">
            <p className="text-sm text-red-400/70">{error}</p>
            <Button variant="outline" size="sm" onClick={onRender}>
              {t("retry")}
            </Button>
          </div>
        ) : !imageData && !isRendering ? (
          <div className="flex h-full flex-col items-center justify-center gap-4 p-8">
            <ZoomIn className="h-12 w-12 text-white/[0.08]" />
            <p className="text-sm text-white/25 tracking-wide">
              {t("clickToRender")}
            </p>
          </div>
        ) : isRendering ? (
          <div className="flex h-full items-center justify-center">
            <div className="text-center">
              <LoadingSpinner className="mx-auto h-8 w-8" />
              <p className="mt-4 text-sm text-white/30">
                {t("computing")}
              </p>
            </div>
          </div>
        ) : (
          <canvas
            ref={canvasRef}
            width={displayW}
            height={displayH}
            className="h-full w-full object-contain"
          />
        )}
      </div>
    </div>
  );
}
