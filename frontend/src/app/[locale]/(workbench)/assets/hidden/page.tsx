"use client";

import { useEffect, useState } from "react";
import { useLocale, useTranslations } from "next-intl";
import { Button } from "@/components/ui/button";
import { Link } from "@/i18n/navigation";
import { CARD_GRID_STYLE } from "@/lib/utils/layout";
import { platform, PlatformApiError, type Asset } from "@/lib/api/platform";

export default function HiddenAssetsPage() {
  const t = useTranslations("commerce");
  const locale = useLocale();
  const [assets, setAssets] = useState<Asset[]>([]);
  const [error, setError] = useState<string | null>(null);
  const [busyAsset, setBusyAsset] = useState<string | null>(null);

  const errorText = (reason: unknown): string => reason instanceof PlatformApiError
    ? t("errors.requestWithCode", { code: reason.code })
    : t("errors.requestFailed");
  const refresh = () => void platform.assets.list("hidden").then((value) => {
    setAssets(value.data);
    setError(null);
  }).catch((reason: unknown) => setError(errorText(reason)));
  useEffect(refresh, []);

  const mutate = (assetId: string, action: Promise<unknown>) => {
    setBusyAsset(assetId);
    void action.then(refresh).catch((reason: unknown) => setError(errorText(reason))).finally(() => setBusyAsset(null));
  };

  return <div className="space-y-5">
    <div className="flex flex-wrap items-start justify-between gap-3">
      <div><h1 className="text-2xl font-semibold">{t("assets.hiddenTitle")}</h1><p className="text-muted-foreground">{t("assets.hiddenSubtitle")}</p></div>
      <Button asChild size="sm" variant="outline"><Link href="/assets">{t("actions.backToLibrary")}</Link></Button>
    </div>
    {error && <p className="text-red-400">{error}</p>}
    {assets.length === 0 && <p className="rounded border border-dashed p-6 text-muted-foreground">{t("assets.hiddenEmpty")}</p>}
    <div className="grid gap-4 lg:gap-5" style={CARD_GRID_STYLE}>{assets.map((asset) => {
      const previewUrl = asset.preview?.thumbnailUrl ?? asset.preview?.videoPosterUrl;
      return <article key={asset.id} className="min-w-0 overflow-hidden rounded-xl border border-white/10 text-sm">
        <div className="aspect-[4/3] border-b border-white/10 bg-white/[0.03]">
          {previewUrl ? <img alt={t("assets.previewAlt", { type: t(`media.${asset.mediaType}`) })} className="block h-full w-full object-contain" src={previewUrl} /> : <div className="flex h-full items-center justify-center p-4 text-center text-muted-foreground">{asset.derivativeStatus === "pending" ? t("assets.previewPreparing") : t("assets.previewUnavailable")}</div>}
        </div>
        <div className="p-4">
          <div className="flex justify-between gap-3"><b>{t(`media.${asset.mediaType}`)}</b><span>{t(`assetStatus.${asset.status}`)}</span></div>
          <p className="mt-2 font-mono text-xs text-muted-foreground" title={asset.id}>{asset.id.slice(0, 13)}…</p>
          <p className="mt-1 text-xs text-muted-foreground">{t(`visibility.${asset.visibility}`)} · {t("assets.created", { date: new Intl.DateTimeFormat(locale, { dateStyle: "medium" }).format(new Date(asset.createdAt)) })}</p>
          <div className="mt-3 flex flex-wrap gap-2">
            <Button size="sm" disabled={busyAsset === asset.id} onClick={() => mutate(asset.id, platform.assets.setVisibility(asset.id, "private"))}>{t("actions.restore")}</Button>
            <Button size="sm" variant="outline" disabled={busyAsset === asset.id} onClick={() => mutate(asset.id, platform.assets.remove(asset.id))}>{t("actions.delete")}</Button>
          </div>
        </div>
      </article>;
    })}</div>
  </div>;
}
