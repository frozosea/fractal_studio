"use client";

import { useState } from "react";
import { useTranslations } from "next-intl";
import { Card, CardContent, CardHeader, CardTitle } from "@/components/ui/card";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "@/components/ui/select";
import { Badge } from "@/components/ui/badge";
import { useSpecialPointsSearch, useSpecialPointsResults, useSpecialPointsEnumerate } from "@/lib/hooks/use-points";
import { useStudioStore } from "@/stores/studio-store";
import type { SpecialPointEnumResult } from "@/types/points";
import { Search, Play } from "lucide-react";

export function PointSearchPanel() {
  const t = useTranslations("points");
  const store = useStudioStore();
  const searchMutation = useSpecialPointsSearch();
  const enumMutation = useSpecialPointsEnumerate();

  const [periodMin, setPeriodMin] = useState(1);
  const [periodMax, setPeriodMax] = useState(6);
  const [preperiodMin, setPreperiodMin] = useState(0);
  const [preperiodMax, setPreperiodMax] = useState(2);
  const [kind, setKind] = useState<string>("center");
  const [results, setResults] = useState<SpecialPointEnumResult[]>([]);
  const [runId, setRunId] = useState<string | null>(null);
  const [searchRunId, setSearchRunId] = useState<string | null>(null);

  const { data: searchResults } = useSpecialPointsResults(searchRunId ?? "");
  const { data: enumResults } = useSpecialPointsResults(runId ?? "");

  // Show results from whichever source has data
  const displayResults = searchResults?.points ?? enumResults?.points ?? results;

  return (
    <div className="space-y-4">
      {/* Search Controls */}
      <Card>
        <CardHeader className="pb-2">
          <CardTitle className="text-sm font-medium">{t("searchTitle")}</CardTitle>
        </CardHeader>
        <CardContent className="space-y-3">
          <div>
            <label className="text-xs text-muted-foreground">{t("kind")}</label>
            <Select value={kind} onValueChange={setKind}>
              <SelectTrigger className="h-8 text-xs">
                <SelectValue />
              </SelectTrigger>
              <SelectContent>
                <SelectItem value="center" className="text-xs">{t("center")}</SelectItem>
                <SelectItem value="misiurewicz" className="text-xs">{t("misiurewicz")}</SelectItem>
              </SelectContent>
            </Select>
          </div>
          <div className="grid grid-cols-2 gap-2">
            <div>
              <label className="text-xs text-muted-foreground">{t("periodMin")}</label>
              <Input type="number" value={periodMin} onChange={(e) => setPeriodMin(Number(e.target.value))} min={1} className="h-8 text-xs" />
            </div>
            <div>
              <label className="text-xs text-muted-foreground">{t("periodMax")}</label>
              <Input type="number" value={periodMax} onChange={(e) => setPeriodMax(Number(e.target.value))} min={1} className="h-8 text-xs" />
            </div>
            <div>
              <label className="text-xs text-muted-foreground">{t("preperiodMin")}</label>
              <Input type="number" value={preperiodMin} onChange={(e) => setPreperiodMin(Number(e.target.value))} min={0} className="h-8 text-xs" />
            </div>
            <div>
              <label className="text-xs text-muted-foreground">{t("preperiodMax")}</label>
              <Input type="number" value={preperiodMax} onChange={(e) => setPreperiodMax(Number(e.target.value))} min={0} className="h-8 text-xs" />
            </div>
          </div>
          <div className="flex gap-2">
            <Button
              variant="fractal"
              size="sm"
              onClick={async () => {
                try {
                  const r = await searchMutation.mutateAsync({
                    req: {
                      kind: kind as "center" | "misiurewicz",
                      periodMin, periodMax,
                      preperiodMin, preperiodMax,
                      visibleOnly: true,
                      viewport: {
                        centerRe: store.centerRe,
                        centerIm: store.centerIm,
                        scale: store.scale,
                        width: store.width,
                        height: store.height,
                      },
                    },
                  });
                  setResults(r.points);
                  setSearchRunId(r.runId);
                } catch { /* handled */ }
              }}
              disabled={searchMutation.isPending}
            >
              <Search className="h-3 w-3" />
              {searchMutation.isPending ? t("searching") : t("search")}
            </Button>
            <Button
              variant="outline"
              size="sm"
              onClick={async () => {
                try {
                  const r = await enumMutation.mutateAsync({
                    kind: kind as "center" | "misiurewicz",
                    periodMin, periodMax,
                    preperiodMin, preperiodMax,
                  });
                  setResults(r.points);
                  setRunId(r.runId);
                } catch { /* handled */ }
              }}
              disabled={enumMutation.isPending}
            >
              <Play className="h-3 w-3" />
              {enumMutation.isPending ? t("enumerating") : t("enumerate")}
            </Button>
          </div>
        </CardContent>
      </Card>

      {/* Results */}
      {displayResults.length > 0 && (
        <Card>
          <CardHeader className="pb-2">
            <CardTitle className="text-sm font-medium">
              {t("results")} ({displayResults.length})
            </CardTitle>
          </CardHeader>
          <CardContent>
            <div className="max-h-96 space-y-1 overflow-y-auto">
              {displayResults.map((point, i) => (
                <div
                  key={point.id ?? i}
                  className="flex items-center justify-between rounded border border-white/5 p-2 hover:bg-white/5"
                >
                  <div className="flex items-center gap-2">
                    <Badge variant={point.kind === "center" ? "fractal" : "neon"} className="text-xs">
                      {point.kind}
                    </Badge>
                    <span className="font-mono text-xs">
                      ({point.re?.toFixed(6)}, {point.im?.toFixed(6)})
                    </span>
                  </div>
                  <div className="flex items-center gap-2 text-xs text-muted-foreground">
                    <span>p={point.period}</span>
                    <span>k={point.preperiod}</span>
                    {point.residual !== undefined && (
                      <span>ε={point.residual.toExponential(1)}</span>
                    )}
                    <Badge variant={point.accepted ? "success" : "destructive"} className="text-xs">
                      {point.accepted ? "ok" : "rejected"}
                    </Badge>
                  </div>
                </div>
              ))}
            </div>
          </CardContent>
        </Card>
      )}
    </div>
  );
}
