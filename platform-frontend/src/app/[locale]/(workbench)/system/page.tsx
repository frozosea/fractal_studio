"use client";

import { useTranslations } from "next-intl";
import { useQuery } from "@tanstack/react-query";
import { getApiClient } from "@/lib/api/client";
import { Card, CardContent, CardHeader, CardTitle } from "@/components/ui/card";
import { Badge } from "@/components/ui/badge";
import { LoadingSpinner } from "@/components/shared/loading-spinner";
import { Cpu, Monitor, HardDrive, Zap } from "lucide-react";

export default function SystemPage() {
  const t = useTranslations("system");
  const { data: hw, isLoading: hwLoading } = useQuery({
    queryKey: ["system", "hardware"],
    queryFn: () => getApiClient().system.hardware(),
    refetchInterval: 30000,
  });
  const { data: caps, isLoading: capsLoading } = useQuery({
    queryKey: ["system", "capabilities"],
    queryFn: () => getApiClient().system.capabilities(),
    refetchInterval: 30000,
  });
  const { data: check } = useQuery({
    queryKey: ["system", "check"],
    queryFn: () => getApiClient().system.check(),
    refetchInterval: 15000,
  });

  if (hwLoading || capsLoading) return <LoadingSpinner />;

  return (
    <div className="space-y-6">
      <h1 className="text-2xl font-bold tracking-tight">{t("title")}</h1>

      <div className="grid grid-cols-2 gap-4">
        {/* Backend Status */}
        <Card>
          <CardHeader>
            <CardTitle className="flex items-center gap-2">
              <Monitor className="h-5 w-5" />
              {t("backendStatus")}
            </CardTitle>
          </CardHeader>
          <CardContent className="space-y-2">
            <div className="flex items-center gap-2">
              <span className={`h-2 w-2 rounded-full ${check ? "bg-green-500 animate-pulse-dot" : "bg-red-500"}`} />
              <span>{check ? t("connected") : t("disconnected")}</span>
            </div>
            {check && (
              <div className="flex gap-2">
                <Badge variant={check.openmp ? "success" : "secondary"}>
                  OpenMP: {check.openmp ? t("enabled") : t("disabled")}
                </Badge>
                <Badge variant={check.cuda ? "success" : "secondary"}>
                  CUDA: {check.cuda ? t("enabled") : t("disabled")}
                </Badge>
              </div>
            )}
          </CardContent>
        </Card>

        {/* CPU */}
        <Card>
          <CardHeader>
            <CardTitle className="flex items-center gap-2">
              <Cpu className="h-5 w-5" />
              {t("cpu")}
            </CardTitle>
          </CardHeader>
          <CardContent className="space-y-1 text-sm">
            {hw && (
              <>
                <p>{hw.cpuModel}</p>
                <p className="text-muted-foreground">
                  {t("logicalCores")}: {hw.cpuLogicalCores} | {t("physicalCores")}: {hw.cpuPhysicalCores}
                </p>
              </>
            )}
          </CardContent>
        </Card>

        {/* GPU */}
        <Card>
          <CardHeader>
            <CardTitle className="flex items-center gap-2">
              <Zap className="h-5 w-5" />
              {t("gpu")}
            </CardTitle>
          </CardHeader>
          <CardContent className="space-y-1 text-sm">
            {hw ? (
              <>
                <p>{hw.gpuModel || t("noGpu")}</p>
                {hw.gpuMemory && <p className="text-muted-foreground">{t("memory")}: {hw.gpuMemory}</p>}
              </>
            ) : (
              <p className="text-muted-foreground">{t("noGpu")}</p>
            )}
          </CardContent>
        </Card>

        {/* Memory */}
        <Card>
          <CardHeader>
            <CardTitle className="flex items-center gap-2">
              <HardDrive className="h-5 w-5" />
              {t("memory")}
            </CardTitle>
          </CardHeader>
          <CardContent className="space-y-1 text-sm">
            {hw && (
              <>
                <p>{t("total")}: {hw.memoryTotalMiB} MiB</p>
                <p className="text-muted-foreground">{t("available")}: {hw.memoryAvailableMiB} MiB</p>
              </>
            )}
          </CardContent>
        </Card>
      </div>

      {/* Capabilities JSON dump */}
      {caps && (
        <Card>
          <CardHeader><CardTitle>{t("capabilities")}</CardTitle></CardHeader>
          <CardContent>
            <pre className="max-h-96 overflow-auto rounded bg-deep-slate p-4 text-xs text-muted-foreground">
              {JSON.stringify(caps, null, 2)}
            </pre>
          </CardContent>
        </Card>
      )}
    </div>
  );
}
