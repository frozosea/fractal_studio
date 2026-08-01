"use client";

import { useTranslations } from "next-intl";
import { X } from "lucide-react";
import { Button } from "@/components/ui/button";
import { COLOR_MAPS } from "@/lib/studio-catalog";
import type { FacetCount, FacetName } from "@/lib/api/platform";
import { cn } from "@/lib/utils/cn";

export type FacetSelection = Partial<Record<FacetName, string | null>>;

/** Row order, and which axes exist at all. */
const FACET_ORDER: readonly FacetName[] = ["variant", "colorMap", "depth", "resolution"] as const;

const COLOR_MAP_PREVIEWS = new Map(COLOR_MAPS.map((entry) => [entry.id as string, entry.preview]));

interface FacetBarProps {
  facets: FacetCount[];
  selection: FacetSelection;
  onChange: (selection: FacetSelection) => void;
  disabled?: boolean;
}

/**
 * Chip rows for the four render facets.
 *
 * Each row scrolls horizontally on its own rather than letting the page scroll
 * sideways, which is the rule the rest of the mobile work follows. Only values
 * the catalogue actually contains are offered, so a chip never promises an
 * empty result.
 */
export function FacetBar({ facets, selection, onChange, disabled }: FacetBarProps) {
  const t = useTranslations("studio");
  const tCommerce = useTranslations("commerce");
  const tFacets = useTranslations("commerce.marketplace.facets");

  const rows = FACET_ORDER.map((facet) => ({
    facet,
    values: facets.filter((entry) => entry.facet === facet),
  })).filter((row) => row.values.length > 0);

  if (rows.length === 0) return null;

  const active = FACET_ORDER.some((facet) => selection[facet]);

  const label = (facet: FacetName, value: string): string => {
    if (facet === "variant") {
      if (value === "custom") return tCommerce("render.customFormula");
      return t.has(`variants.${value}.name`) ? t(`variants.${value}.name`) : value;
    }
    if (facet === "colorMap") {
      if (value === "custom_gradient") return t("customGradient");
      return t.has(`colorMaps.${value}.name`) ? t(`colorMaps.${value}.name`) : value;
    }
    return tFacets(`${facet}Values.${value}`);
  };

  return (
    <div className="space-y-2">
      {rows.map(({ facet, values }) => (
        <div key={facet} className="flex items-center gap-3">
          <span className="w-16 shrink-0 font-mono text-[10px] uppercase tracking-[0.14em] text-ink/30">
            {tFacets(facet)}
          </span>
          {/* Only this row scrolls; the page never moves sideways. */}
          <div className="-mx-1 flex min-w-0 flex-1 gap-1.5 overflow-x-auto px-1 py-0.5">
            {values.map(({ value, count }) => {
              const isActive = selection[facet] === value;
              const swatch = facet === "colorMap" ? COLOR_MAP_PREVIEWS.get(value) : undefined;
              return (
                <button
                  key={value}
                  type="button"
                  disabled={disabled}
                  aria-pressed={isActive}
                  onClick={() => onChange({ ...selection, [facet]: isActive ? null : value })}
                  className={cn(
                    "flex shrink-0 items-center gap-1.5 whitespace-nowrap rounded-full border px-3 py-1 text-xs transition-colors duration-150 disabled:opacity-40 coarse:py-2",
                    isActive
                      ? "border-amber-400/60 bg-amber-400/10 text-amber-200"
                      : "border-hairline/10 text-ink/55 hover:border-hairline/20 hover:text-ink/80",
                  )}
                >
                  {swatch && (
                    <span
                      aria-hidden="true"
                      className="h-2.5 w-4 shrink-0 rounded-sm border border-hairline/10"
                      style={{ background: swatch }}
                    />
                  )}
                  {label(facet, value)}
                  <span className="text-ink/30">{count}</span>
                </button>
              );
            })}
          </div>
        </div>
      ))}

      {active && (
        <Button
          size="sm"
          variant="ghost"
          disabled={disabled}
          className="coarse:h-10"
          onClick={() => onChange({})}
        >
          <X className="h-3.5 w-3.5" />
          {tCommerce("actions.clearFilters")}
        </Button>
      )}
    </div>
  );
}
