"use client";

import { useState } from "react";
import { useTranslations } from "next-intl";
import { useRuns } from "@/lib/hooks/use-runs";
import { RunsTable } from "@/components/runs/runs-table";
import { LoadingSpinner } from "@/components/shared/loading-spinner";
import { ErrorDisplay } from "@/components/shared/error-display";
import { EmptyState } from "@/components/shared/empty-state";
import { Button } from "@/components/ui/button";
import { ListTodo } from "lucide-react";

const PAGE_SIZE = 50;

export default function RunsPage() {
  const t = useTranslations("runs");
  const common = useTranslations("common");
  const [page, setPage] = useState(0);
  const { data, isLoading, isError, error, refetch } = useRuns({
    limit: PAGE_SIZE,
    offset: page * PAGE_SIZE,
  });

  if (isLoading) return <LoadingSpinner />;
  if (isError) {
    return (
      <ErrorDisplay
        message={error?.message ?? common("error")}
        onRetry={() => refetch()}
      />
    );
  }
  if (!data || data.items.length === 0) {
    return (
      <EmptyState
        icon={<ListTodo className="h-12 w-12" />}
        title={t("emptyTitle")}
        description={t("emptyDescription")}
      />
    );
  }

  const totalPages = Math.max(1, Math.ceil(data.totalCount / PAGE_SIZE));

  return (
    <div className="space-y-6">
      <div className="flex items-center justify-between">
        <h1 className="text-2xl font-bold tracking-tight">{t("title")}</h1>
        <span className="text-sm text-muted-foreground">
          {data.totalCount} {t("totalRuns")}
        </span>
      </div>
      <RunsTable items={data.items} />
      <div className="flex items-center justify-between">
        <Button
          variant="outline"
          size="sm"
          onClick={() => setPage((p) => Math.max(0, p - 1))}
          disabled={page === 0}
        >
          {common("previous")}
        </Button>
        <span className="text-sm text-muted-foreground">
          {page + 1} / {totalPages}
        </span>
        <Button
          variant="outline"
          size="sm"
          onClick={() => setPage((p) => p + 1)}
          disabled={page + 1 >= totalPages}
        >
          {common("next")}
        </Button>
      </div>
    </div>
  );
}
