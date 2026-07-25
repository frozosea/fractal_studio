"use client";

import { useState } from "react";
import { useTranslations } from "next-intl";
import { Card, CardContent, CardHeader, CardTitle } from "@/components/ui/card";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { Badge } from "@/components/ui/badge";
import { LoadingSpinner } from "@/components/shared/loading-spinner";
import { ErrorDisplay } from "@/components/shared/error-display";
import { useVariants, useCompileVariant, useDeleteVariant } from "@/lib/hooks/use-variants";
import { FlaskConical, Trash2 } from "lucide-react";

export default function VariantsPage() {
  const t = useTranslations("variants");
  const common = useTranslations("common");
  const { data, isLoading, isError, error, refetch } = useVariants();
  const compileMutation = useCompileVariant();
  const deleteMutation = useDeleteVariant();
  const [formula, setFormula] = useState("z*z + c");
  const [name, setName] = useState("");
  const [bailout, setBailout] = useState("");

  if (isLoading) return <LoadingSpinner />;
  if (isError) return <ErrorDisplay message={error?.message ?? common("error")} onRetry={() => refetch()} />;

  return (
    <div className="space-y-6">
      <h1 className="text-2xl font-bold tracking-tight">{t("title")}</h1>

      {/* Compile new variant */}
      <Card>
        <CardHeader><CardTitle>{t("compileNew")}</CardTitle></CardHeader>
        <CardContent className="space-y-4">
          <div className="grid grid-cols-3 gap-4">
            <div>
              <label className="text-sm text-muted-foreground">{t("name")}</label>
              <Input value={name} onChange={(e) => setName(e.target.value)} placeholder="MyVariant" />
            </div>
            <div>
              <label className="text-sm text-muted-foreground">{t("formula")}</label>
              <Input value={formula} onChange={(e) => setFormula(e.target.value)} placeholder="z*z + c" />
            </div>
            <div>
              <label className="text-sm text-muted-foreground">{t("bailout")} ({t("optional")})</label>
              <Input value={bailout} onChange={(e) => setBailout(e.target.value)} placeholder="2.0" type="number" step={0.1} />
            </div>
          </div>
          <Button
            variant="fractal"
            onClick={async () => {
              try {
                await compileMutation.mutateAsync({
                  name: name || "Unnamed",
                  formula,
                  bailout: bailout ? Number(bailout) : undefined,
                });
                setName("");
                setFormula("z*z + c");
                setBailout("");
              } catch { /* handled by query */ }
            }}
            disabled={compileMutation.isPending || !formula}
          >
            <FlaskConical className="h-4 w-4" />
            {compileMutation.isPending ? t("compiling") : t("compile")}
          </Button>
          {compileMutation.data && (
            <div className="rounded-lg border border-white/10 p-3">
              {compileMutation.data.ok ? (
                <p className="text-green-400">{t("compileSuccess")}: {compileMutation.data.variantId}</p>
              ) : (
                <p className="text-red-400">{compileMutation.data.error || t("compileFailed")}</p>
              )}
            </div>
          )}
        </CardContent>
      </Card>

      {/* Built-in variants */}
      <Card>
        <CardHeader><CardTitle>{t("builtinVariants")}</CardTitle></CardHeader>
        <CardContent>
          <div className="flex flex-wrap gap-2">
            {data?.builtin.map((v) => (
              <Badge key={v.variantId} variant="secondary">{v.name}</Badge>
            ))}
          </div>
        </CardContent>
      </Card>

      {/* Custom variants */}
      <Card>
        <CardHeader><CardTitle>{t("customVariants")}</CardTitle></CardHeader>
        <CardContent>
          {!data?.custom.length ? (
            <p className="text-sm text-muted-foreground">{t("noCustom")}</p>
          ) : (
            <div className="space-y-2">
              {data.custom.map((v) => (
                <div key={v.variantId} className="flex items-center justify-between rounded-lg border border-white/10 p-3">
                  <div>
                    <span className="font-mono text-sm">{v.variantId}</span>
                    <span className="ml-3 text-sm text-muted-foreground">{v.name}</span>
                    <span className="ml-3 text-xs text-muted-foreground">formula: {v.formula}</span>
                    <span className="ml-3 text-xs text-muted-foreground">bailout: {v.bailout}</span>
                  </div>
                  <div className="flex items-center gap-2">
                    <Badge variant={v.loaded ? "success" : "error"}>
                      {v.loaded ? t("loaded") : t("notLoaded")}
                    </Badge>
                    <Button
                      variant="ghost"
                      size="icon"
                      onClick={() => deleteMutation.mutate(v.variantId)}
                      disabled={deleteMutation.isPending}
                    >
                      <Trash2 className="h-4 w-4 text-destructive" />
                    </Button>
                  </div>
                </div>
              ))}
            </div>
          )}
        </CardContent>
      </Card>
    </div>
  );
}
