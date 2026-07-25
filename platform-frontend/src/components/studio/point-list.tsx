"use client";

import { useTranslations } from "next-intl";
import { Badge } from "@/components/ui/badge";
import type { SpecialPoint } from "@/types/points";

interface PointListProps {
  points: SpecialPoint[];
}

export function PointList({ points }: PointListProps) {
  const t = useTranslations("points");

  if (points.length === 0) {
    return <p className="text-sm text-muted-foreground">{t("noPoints")}</p>;
  }

  return (
    <div className="space-y-2">
      {points.map((point) => (
        <div
          key={point.id}
          className="flex items-center justify-between rounded-lg border border-white/10 p-3"
        >
          <div className="flex items-center gap-3">
            <Badge variant={point.pointType === "center" ? "fractal" : "neon"}>
              {point.pointType}
            </Badge>
            <span className="font-mono text-sm">
              ({point.real?.toFixed(8)}, {point.imag?.toFixed(8)})
            </span>
            <span className="text-xs text-muted-foreground">
              k={point.k} p={point.p}
            </span>
          </div>
          <span className="text-xs text-muted-foreground">{point.family}</span>
        </div>
      ))}
    </div>
  );
}
