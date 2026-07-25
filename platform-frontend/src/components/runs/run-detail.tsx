"use client";

import { useTranslations } from "next-intl";
import { Card, CardContent, CardHeader, CardTitle } from "@/components/ui/card";
import { Badge } from "@/components/ui/badge";
import { Button } from "@/components/ui/button";
import { ProgressBar } from "@/components/runs/progress-bar";
import { ArtifactList } from "@/components/runs/artifact-list";
import { formatDate } from "@/lib/utils/format";
import type { RunStatusResponse } from "@/types/runs";
import { XCircle } from "lucide-react";

const statusVariant: Record<string, "success" | "warning" | "error" | "info" | "running" | "secondary"> = {
  completed: "success",
  running: "running",
  queued: "warning",
  cancelled: "secondary",
  failed: "error",
};

interface RunDetailProps {
  run: RunStatusResponse;
  onCancel: () => void;
  isCancelling: boolean;
}

export function RunDetail({ run, onCancel, isCancelling }: RunDetailProps) {
  const t = useTranslations("runs");
  const canCancel =
    !["completed", "failed", "cancelled"].includes(run.status) &&
    !run.cancelRequested;

  return (
    <div className="grid grid-cols-3 gap-6">
      {/* Status card */}
      <Card className="col-span-2">
        <CardHeader>
          <div className="flex items-center justify-between">
            <CardTitle>{t("runDetail")}</CardTitle>
            <div className="flex items-center gap-2">
              <Badge variant={statusVariant[run.status] ?? "secondary"}>
                {run.status}
              </Badge>
              {canCancel && (
                <Button
                  variant="destructive"
                  size="sm"
                  onClick={onCancel}
                  disabled={isCancelling}
                >
                  <XCircle className="h-4 w-4" />
                  {isCancelling ? t("cancelling") : t("cancel")}
                </Button>
              )}
            </div>
          </div>
        </CardHeader>
        <CardContent className="space-y-4">
          <div className="grid grid-cols-2 gap-4 text-sm">
            <div>
              <span className="text-muted-foreground">{t("runId")}: </span>
              <span className="font-mono">{run.id}</span>
            </div>
            <div>
              <span className="text-muted-foreground">{t("module")}: </span>
              <span>{run.module}</span>
            </div>
            <div>
              <span className="text-muted-foreground">{t("startedAt")}: </span>
              <span>{run.startedAt ? formatDate(new Date(run.startedAt * 1000).toISOString()) : "-"}</span>
            </div>
            <div>
              <span className="text-muted-foreground">{t("finishedAt")}: </span>
              <span>{run.finishedAt ? formatDate(new Date(run.finishedAt * 1000).toISOString()) : "-"}</span>
            </div>
          </div>

          {/* Progress */}
          {run.progress && Object.keys(run.progress).length > 0 && (
            <div className="space-y-2">
              <div className="flex items-center justify-between text-xs text-muted-foreground">
                <span>
                  {run.progress.stage} {run.progress.engine ? `(${run.progress.engine}/${run.progress.scalar})` : ""}
                </span>
                <span>
                  {run.progress.current ?? 0}/{run.progress.total ?? 0}
                </span>
              </div>
              <ProgressBar percent={run.progress.percent ?? 0} />
              {run.progress.elapsedMs !== undefined && (
                <p className="text-xs text-muted-foreground">
                  {t("elapsed")}: {(run.progress.elapsedMs / 1000).toFixed(1)}s
                  {run.progress.estimatedRemainingMs != null && (
                    <> | {t("remaining")}: {(run.progress.estimatedRemainingMs / 1000).toFixed(1)}s</>
                  )}
                </p>
              )}
              {run.progress.errorMessage && (
                <p className="text-xs text-red-400">{run.progress.errorMessage}</p>
              )}
            </div>
          )}
        </CardContent>
      </Card>

      {/* Artifacts card */}
      <Card>
        <CardHeader>
          <CardTitle>{t("artifacts")}</CardTitle>
        </CardHeader>
        <CardContent>
          <ArtifactList artifacts={run.artifacts} />
        </CardContent>
      </Card>
    </div>
  );
}
