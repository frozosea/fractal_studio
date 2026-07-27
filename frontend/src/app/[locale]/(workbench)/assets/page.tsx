"use client";

import { useEffect, useState } from "react";
import { useLocale, useTranslations } from "next-intl";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { toast } from "@/components/ui/toaster";
import { useRouter } from "@/i18n/navigation";
import { platform, PlatformApiError, type Asset } from "@/lib/api/platform";

export default function AssetsPage() {
  const t = useTranslations("commerce");
  const locale = useLocale();
  const router = useRouter();
  const [assets, setAssets] = useState<Asset[]>([]);
  const [listingAsset, setListingAsset] = useState<string | null>(null);
  const [title, setTitle] = useState("");
  const [price, setPrice] = useState("19.90");
  const [error, setError] = useState<string | null>(null);

  const errorText = (reason: unknown): string => {
    if (reason instanceof PlatformApiError && reason.status === 403) return t("errors.creatorRequired");
    if (reason instanceof PlatformApiError) return t("errors.requestWithCode", { code: reason.code });
    return t("errors.requestFailed");
  };
  const refresh = () => void platform.assets.list().then((value) => {
    setAssets(value.data);
    setError(null);
  }).catch((reason: unknown) => setError(errorText(reason)));
  useEffect(refresh, []);

  const download = async (assetId: string) => {
    try { window.open((await platform.assets.downloadUrl(assetId)).url, "_blank", "noopener,noreferrer"); }
    catch (reason) { setError(errorText(reason)); }
  };

  const createListing = async () => {
    if (!listingAsset) return;
    try {
      const listing = await platform.marketplace.create({ assetId: listingAsset, title, description: "", tags: ["fractal"], price, licenceOffer: { code: "personal", termsVersion: "v1" } });
      toast({
        title: t("assets.draftCreated"),
        description: t("assets.draftCreatedDescription", { title: listing.title }),
        variant: "success",
      });
      router.push("/listings");
    } catch (reason) { setError(errorText(reason)); }
  };

  return <div className="space-y-5">
    <div><h1 className="text-2xl font-semibold">{t("assets.title")}</h1><p className="text-muted-foreground">{t("assets.subtitle")}</p></div>
    {error && <p className="text-red-400">{error}</p>}
    {assets.length === 0 && <p className="rounded border border-dashed p-6 text-muted-foreground">{t("assets.empty")}</p>}
    <div className="grid gap-4 sm:grid-cols-2 xl:grid-cols-3">{assets.map((asset) => {
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
            {asset.status === "ready" && <Button size="sm" onClick={() => void download(asset.id)}>{t("actions.download")}</Button>}
            {asset.status === "ready" && asset.derivativeStatus === "ready" && <Button size="sm" variant="outline" onClick={() => setListingAsset(asset.id)}>{t("actions.createListing")}</Button>}
            <Button size="sm" variant="outline" onClick={() => void platform.assets.setVisibility(asset.id, asset.visibility === "private" ? "hidden" : "private").then(refresh).catch((reason: unknown) => setError(errorText(reason)))}>{asset.visibility === "private" ? t("actions.hide") : t("actions.restore")}</Button>
            <Button size="sm" variant="outline" onClick={() => void platform.assets.remove(asset.id).then(refresh).catch((reason: unknown) => setError(errorText(reason)))}>{t("actions.delete")}</Button>
          </div>
        </div>
      </article>;
    })}</div>
    {listingAsset && <section className="max-w-lg rounded-xl border border-primary/30 p-4"><h2 className="mb-3 text-lg font-medium">{t("assets.createDraft")}</h2><p className="mb-3 text-sm text-muted-foreground">{t("assets.draftHint")}</p><Input placeholder={t("assets.listingTitle")} value={title} onChange={(event) => setTitle(event.target.value)} /><Input className="mt-2" placeholder={t("assets.priceCny")} value={price} onChange={(event) => setPrice(event.target.value)} /><div className="mt-3 flex gap-2"><Button onClick={() => void createListing()} disabled={!title}>{t("actions.createDraft")}</Button><Button variant="outline" onClick={() => setListingAsset(null)}>{t("actions.cancel")}</Button></div></section>}
  </div>;
}
