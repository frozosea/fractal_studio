"use client";

import { useParams } from "next/navigation";
import { useTranslations } from "next-intl";
import { useRunStatus, useCancelRun } from "@/lib/hooks/use-runs";
import { RunDetail } from "@/components/runs/run-detail";
import { LoadingSpinner } from "@/components/shared/loading-spinner";
import { ErrorDisplay } from "@/components/shared/error-display";
import { Button } from "@/components/ui/button";
import { ArrowLeft } from "lucide-react";
import { Link } from "@/i18n/navigation";

export default function RunDetailPage() {
  const { runId } = useParams<{ runId: string }>();
  const t = useTranslations("runs");
  const { data, isLoading, isError, error, refetch } = useRunStatus(runId);
  const cancelMutation = useCancelRun();

  if (isLoading) return <LoadingSpinner />;
  if (isError) {
    return (
      <ErrorDisplay
        message={error?.message ?? "Failed to load run"}
        onRetry={() => refetch()}
      />
    );
  }
  if (!data) return null;

  return (
    <div className="space-y-6">
      <div className="flex items-center gap-4">
        <Link href="/runs">
          <Button variant="ghost" size="icon">
            <ArrowLeft className="h-4 w-4" />
          </Button>
        </Link>
        <h1 className="text-2xl font-bold tracking-tight">
          {t("runDetail")}: {data.id.slice(0, 8)}...
        </h1>
      </div>
      <RunDetail
        run={data}
        onCancel={() => cancelMutation.mutate(runId)}
        isCancelling={cancelMutation.isPending}
      />
    </div>
  );
}
