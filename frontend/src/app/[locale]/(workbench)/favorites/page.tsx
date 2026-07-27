"use client";

import { useEffect, useState } from "react";
import { useTranslations } from "next-intl";
import { Button } from "@/components/ui/button";
import { platform, type Listing } from "@/lib/api/platform";

type Favorite = { assetId: string; createdAt: string; listing?: Listing | null };
export default function FavoritesPage() {
  const t = useTranslations("commerce");
  const [items, setItems] = useState<Favorite[]>([]); const [error, setError] = useState<string | null>(null);
  const refresh = () => void platform.marketplace.favorites().then((value) => setItems(value.data)).catch(() => setError(t("errors.requestFailed")));
  useEffect(refresh, []);
  const remove = (assetId: string) => void platform.marketplace.unfavorite(assetId).then(refresh).catch(() => setError(t("errors.requestFailed")));
  return <div className="space-y-5"><div><h1 className="text-2xl font-semibold">{t("favorites.title")}</h1><p className="text-muted-foreground">{t("favorites.subtitle")}</p></div>{error && <p className="text-red-400">{error}</p>}{items.length === 0 && <p className="rounded-xl border border-dashed border-white/15 p-6 text-sm text-muted-foreground">{t("favorites.empty")}</p>}<div className="grid gap-4 sm:grid-cols-2">{items.map((favorite) => <article key={favorite.assetId} className="overflow-hidden rounded-xl border border-white/10">{favorite.listing?.preview?.thumbnailUrl && <div className="aspect-[4/3] bg-white/5"><img alt={t("marketplace.previewAlt", { title: favorite.listing.title })} className="h-full w-full object-cover" src={favorite.listing.preview.thumbnailUrl} /></div>}<div className="flex items-center justify-between gap-3 p-4"><div><b>{favorite.listing?.title ?? t("favorites.unavailable")}</b><p className="text-sm text-muted-foreground">{favorite.listing ? `${favorite.listing.price} CNY · ${favorite.listing.creator.displayName}` : t("favorites.unpublished")}</p></div><Button size="sm" variant="outline" onClick={() => remove(favorite.assetId)}>{t("actions.remove")}</Button></div></article>)}</div></div>;
}
