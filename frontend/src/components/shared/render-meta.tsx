"use client";

import { useTranslations } from "next-intl";
import type { RenderMeta } from "@/lib/api/platform";
import { cn } from "@/lib/utils/cn";

/**
 * The initial viewport width in complex units. Zoom depth is reported relative
 * to it, so "0" means the default view and each step is one power of ten.
 */
const BASE_SCALE = 3;
/** Below this the view is barely moved from the default; not worth a chip. */
const DEEP_ZOOM_THRESHOLD = 1;

interface RenderMetaProps {
  render?: RenderMeta | null;
  className?: string;
}

/**
 * One compact line describing what the artwork actually is: resolution,
 * formula, iteration depth and colouring. Renders nothing when a listing
 * predates the facet columns, rather than showing a row of blanks.
 *
 * Value labels reuse the studio's existing catalogue translations, so no
 * formula or palette name needs translating twice.
 */
export function RenderMeta({ render, className }: RenderMetaProps) {
  const t = useTranslations("studio");
  const tRender = useTranslations("commerce.render");
  if (!render) return null;

  const parts: string[] = [];

  if (render.width && render.height) parts.push(`${render.width}×${render.height}`);

  if (render.variant) {
    // `custom` marks a formula written in the DSL, which has no catalogue entry.
    parts.push(
      render.variant === "custom"
        ? tRender("customFormula")
        : t.has(`variants.${render.variant}.name`)
          ? t(`variants.${render.variant}.name`)
          : render.variant,
    );
  }

  if (render.iterations) parts.push(tRender("iterations", { count: render.iterations }));

  if (render.colorMap) {
    parts.push(
      render.colorMap === "custom_gradient"
        ? t("customGradient")
        : t.has(`colorMaps.${render.colorMap}.name`)
          ? t(`colorMaps.${render.colorMap}.name`)
          : render.colorMap,
    );
  }

  if (parts.length === 0) return null;

  const depth =
    render.viewScale && render.viewScale > 0 ? Math.log10(BASE_SCALE / render.viewScale) : null;

  return (
    <p className={cn("flex flex-wrap items-center gap-x-1.5 gap-y-1 font-mono text-[11px] text-ink/60", className)}>
      {parts.map((part, index) => (
        <span key={`${part}-${index}`} className="whitespace-nowrap">
          {index > 0 && <span className="mr-1.5 text-ink/20">·</span>}
          {part}
        </span>
      ))}
      {depth !== null && depth >= DEEP_ZOOM_THRESHOLD && (
        <span className="whitespace-nowrap border border-amber-300/25 px-1.5 py-px text-amber-200/70">
          {tRender("zoomDepth", { depth: depth.toFixed(1) })}
        </span>
      )}
    </p>
  );
}
