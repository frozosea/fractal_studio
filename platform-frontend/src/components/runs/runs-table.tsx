"use client";

import { useTranslations } from "next-intl";
import { Badge } from "@/components/ui/badge";
import { Button } from "@/components/ui/button";
import { Link } from "@/i18n/navigation";
import type { RunRow } from "@/types/runs";
import { formatDate } from "@/lib/utils/format";

const statusVariant: Record<string, "success" | "warning" | "error" | "info" | "running" | "secondary"> = {
  completed: "success",
  running: "running",
  queued: "warning",
  cancelled: "secondary",
  failed: "error",
};

interface RunsTableProps {
  items: RunRow[];
}

export function RunsTable({ items }: RunsTableProps) {
  const t = useTranslations("runs");

  return (
    <div className="overflow-x-auto rounded-xl border border-white/10">
      <table className="w-full text-sm">
        <thead>
          <tr className="border-b border-white/10 bg-deep-slate/50">
            <th className="px-4 py-3 text-left font-medium text-muted-foreground">
              {t("runId")}
            </th>
            <th className="px-4 py-3 text-left font-medium text-muted-foreground">
              {t("module")}
            </th>
            <th className="px-4 py-3 text-left font-medium text-muted-foreground">
              {t("status")}
            </th>
            <th className="px-4 py-3 text-left font-medium text-muted-foreground">
              {t("startedAt")}
            </th>
            <th className="px-4 py-3 text-left font-medium text-muted-foreground">
              {t("finishedAt")}
            </th>
            <th className="px-4 py-3 text-right font-medium text-muted-foreground">
              {t("actions")}
            </th>
          </tr>
        </thead>
        <tbody>
          {items.map((run) => (
            <tr
              key={run.id}
              className="border-b border-white/5 hover:bg-white/5 transition-colors"
            >
              <td className="px-4 py-3 font-mono text-xs">
                {run.id.slice(0, 8)}...
              </td>
              <td className="px-4 py-3">{run.module}</td>
              <td className="px-4 py-3">
                <Badge variant={statusVariant[run.status] ?? "secondary"}>
                  {run.status}
                </Badge>
                {run.cancelRequested && (
                  <Badge variant="warning" className="ml-1">
                    {t("cancelRequested")}
                  </Badge>
                )}
              </td>
              <td className="px-4 py-3 text-xs text-muted-foreground">
                {run.startedAt ? formatDate(new Date(run.startedAt * 1000).toISOString()) : "-"}
              </td>
              <td className="px-4 py-3 text-xs text-muted-foreground">
                {run.finishedAt ? formatDate(new Date(run.finishedAt * 1000).toISOString()) : "-"}
              </td>
              <td className="px-4 py-3 text-right">
                <Link href={`/runs/${run.id}`}>
                  <Button variant="ghost" size="sm">
                    {t("view")}
                  </Button>
                </Link>
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}
