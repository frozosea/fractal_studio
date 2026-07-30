"use client";

import { useEffect, useState } from "react";
import { useLocale, useTranslations } from "next-intl";
import { Button } from "@/components/ui/button";
import { Dialog, DialogContent, DialogDescription, DialogFooter, DialogHeader, DialogTitle } from "@/components/ui/dialog";
import { Input } from "@/components/ui/input";
import { toast } from "@/components/ui/toaster";
import { Link, useRouter } from "@/i18n/navigation";
import { CARD_GRID_STYLE } from "@/lib/utils/layout";
import { platform, PlatformApiError, type Asset } from "@/lib/api/platform";
import { useAuth } from "@/providers/auth-provider";

export default function AssetsPage() {
  const t = useTranslations("commerce");
  const locale = useLocale();
  const router = useRouter();
  const { user, isPending } = useAuth();
  const [assets, setAssets] = useState<Asset[]>([]);
  const [listingAsset, setListingAsset] = useState<string | null>(null);
  const [title, setTitle] = useState("");
  const [price, setPrice] = useState("19.90");
  const [error, setError] = useState<string | null>(null);
  const [listingError, setListingError] = useState<string | null>(null);
  const [creatingListing, setCreatingListing] = useState(false);

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

  // A listed asset shows where it stands on the marketplace instead of the
  // generic asset status: that is the state the owner acts on here.
  const listingLabel = (asset: Asset): string | null => {
    if (!asset.listingStatus) return null;
    return asset.listingStatus === "published" ? t("listingStatus.published") : t("assets.statusListed");
  };

  // Hiding or deleting a listed asset withdraws its listing server-side, and a
  // published listing disappears from Marketplace with it. Ask first — that is
  // not recoverable from this screen.
  const confirmWithdrawal = (asset: Asset, action: "hide" | "delete"): boolean => {
    if (!asset.listingStatus) return true;
    const key = asset.listingStatus === "published" ? "confirmPublished" : "confirmListed";
    return window.confirm(t(`assets.${key}.${action}`));
  };
  const hide = (asset: Asset) => {
    if (!confirmWithdrawal(asset, "hide")) return;
    void platform.assets.setVisibility(asset.id, "hidden").then(refresh).catch((reason: unknown) => setError(errorText(reason)));
  };
  const remove = (asset: Asset) => {
    if (!confirmWithdrawal(asset, "delete")) return;
    void platform.assets.remove(asset.id).then(refresh).catch((reason: unknown) => setError(errorText(reason)));
  };

  const createListing = async () => {
    if (!listingAsset) return;
    setCreatingListing(true);
    setListingError(null);
    try {
      const listing = await platform.marketplace.create({ assetId: listingAsset, title: title.trim(), description: "", tags: ["fractal"], price, licenceOffer: { code: "personal", termsVersion: "v1" } });
      toast({
        title: t("assets.draftCreated"),
        description: t("assets.draftCreatedDescription", { title: listing.title }),
        variant: "success",
      });
      setListingAsset(null);
      router.push("/listings");
    } catch (reason) {
      setListingError(errorText(reason));
    } finally {
      setCreatingListing(false);
    }
  };

  const openListing = (assetId: string) => {
    if (!user?.roles.includes("creator")) {
      setError(t("errors.creatorRequired"));
      router.push("/payouts");
      return;
    }
    setTitle("");
    setPrice("19.90");
    setListingError(null);
    setListingAsset(assetId);
  };

  return <div className="space-y-5">
    <div className="flex flex-wrap items-start justify-between gap-3">
      <div><h1 className="text-2xl font-semibold">{t("assets.title")}</h1><p className="text-muted-foreground">{t("assets.subtitle")}</p></div>
      <Button asChild size="sm" variant="outline"><Link href="/assets/hidden">{t("actions.hiddenLibrary")}</Link></Button>
    </div>
    {error && <p className="text-red-400">{error}</p>}
    {assets.length === 0 && <p className="rounded border border-dashed p-6 text-muted-foreground">{t("assets.empty")}</p>}
    <div className="grid gap-4 lg:gap-5" style={CARD_GRID_STYLE}>{assets.map((asset) => {
      const previewUrl = asset.preview?.thumbnailUrl ?? asset.preview?.videoPosterUrl;
      return <article key={asset.id} className="min-w-0 overflow-hidden rounded-xl border border-white/10 text-sm">
        <div className="aspect-[4/3] border-b border-white/10 bg-white/[0.03]">
          {previewUrl ? <img alt={t("assets.previewAlt", { type: t(`media.${asset.mediaType}`) })} className="block h-full w-full object-contain" src={previewUrl} /> : <div className="flex h-full items-center justify-center p-4 text-center text-muted-foreground">{asset.derivativeStatus === "pending" ? t("assets.previewPreparing") : t("assets.previewUnavailable")}</div>}
        </div>
        <div className="p-4">
          <div className="flex justify-between gap-3"><b>{t(`media.${asset.mediaType}`)}</b><span>{listingLabel(asset) ?? t(`assetStatus.${asset.status}`)}</span></div>
          <p className="mt-2 font-mono text-xs text-muted-foreground" title={asset.id}>{asset.id.slice(0, 13)}…</p>
          <p className="mt-1 text-xs text-muted-foreground">{t(`visibility.${asset.visibility}`)} · {t("assets.created", { date: new Intl.DateTimeFormat(locale, { dateStyle: "medium" }).format(new Date(asset.createdAt)) })}</p>
          <div className="mt-3 flex flex-wrap gap-2">
            {asset.status === "ready" && <Button size="sm" onClick={() => void download(asset.id)}>{t("actions.download")}</Button>}
            {asset.status === "ready" && asset.derivativeStatus === "ready" && !asset.listingStatus && <Button size="sm" variant="outline" disabled={isPending} onClick={() => openListing(asset.id)}>{t("actions.createListing")}</Button>}
            <Button size="sm" variant="outline" onClick={() => hide(asset)}>{t("actions.hide")}</Button>
            <Button size="sm" variant="outline" onClick={() => remove(asset)}>{t("actions.delete")}</Button>
          </div>
        </div>
      </article>;
    })}</div>
    <Dialog open={Boolean(listingAsset)} onOpenChange={(open) => { if (!open && !creatingListing) setListingAsset(null); }}>
      <DialogContent>
        <DialogHeader>
          <DialogTitle>{t("assets.createDraft")}</DialogTitle>
          <DialogDescription>{t("assets.draftHint")}</DialogDescription>
        </DialogHeader>
        <div className="space-y-3">
          <Input autoFocus placeholder={t("assets.listingTitle")} value={title} maxLength={120} onChange={(event) => setTitle(event.target.value)} />
          <Input placeholder={t("assets.priceCny")} type="number" min="0.01" step="0.01" value={price} onChange={(event) => setPrice(event.target.value)} />
          {listingError && <p className="text-sm text-red-400">{listingError}</p>}
        </div>
        <DialogFooter>
          <Button variant="outline" disabled={creatingListing} onClick={() => setListingAsset(null)}>{t("actions.cancel")}</Button>
          <Button loading={creatingListing} onClick={() => void createListing()} disabled={!title.trim() || !price}>{t("actions.createDraft")}</Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  </div>;
}
