"use client";

import { useTranslations } from "next-intl";
import { getApiClient } from "@/lib/api/client";
import type { RunArtifactStatus } from "@/types/runs";
import { Download, ExternalLink } from "lucide-react";

interface ArtifactListProps {
  artifacts: RunArtifactStatus[];
}

export function ArtifactList({ artifacts }: ArtifactListProps) {
  const t = useTranslations("runs");

  if (artifacts.length === 0) {
    return <p className="text-xs text-muted-foreground">{t("noArtifacts")}</p>;
  }

  return (
    <div className="space-y-2">
      {artifacts.map((a) => (
        <div
          key={a.artifactId}
          className="flex items-center justify-between rounded border border-white/5 p-2"
        >
          <div className="flex-1 min-w-0">
            <p className="truncate text-xs font-mono">{a.name}</p>
            <p className="text-xs text-muted-foreground">{a.kind}</p>
          </div>
          <div className="flex items-center gap-1">
            <a
              href={getApiClient().artifacts.contentUrl(a.artifactId)}
              target="_blank"
              rel="noreferrer"
              className="rounded p-1 hover:bg-white/10"
              title={t("view")}
            >
              <ExternalLink className="h-3 w-3" />
            </a>
            <a
              href={getApiClient().artifacts.downloadUrl(a.artifactId)}
              className="rounded p-1 hover:bg-white/10"
              title={t("download")}
            >
              <Download className="h-3 w-3" />
            </a>
          </div>
        </div>
      ))}
    </div>
  );
}
