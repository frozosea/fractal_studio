"use client";

import { useEffect, useState } from "react";
import { Button } from "@/components/ui/button";
import { platform, type Listing } from "@/lib/api/platform";

type Favorite = { assetId: string; createdAt: string; listing?: Listing | null };
function text(error: unknown): string { return error instanceof Error ? error.message : "Request failed"; }

export default function FavoritesPage() {
  const [items, setItems] = useState<Favorite[]>([]); const [error, setError] = useState<string | null>(null);
  const refresh = () => void platform.marketplace.favorites().then((value) => setItems(value.data)).catch((reason: unknown) => setError(text(reason)));
  useEffect(refresh, []);
  const remove = (assetId: string) => void platform.marketplace.unfavorite(assetId).then(refresh).catch((reason: unknown) => setError(text(reason)));
  return <div className="space-y-5"><div><h1 className="text-2xl font-semibold">Favorites</h1><p className="text-muted-foreground">Saved marketplace artwork.</p></div>{error && <p className="text-red-400">{error}</p>}<div className="space-y-3">{items.map((favorite) => <article key={favorite.assetId} className="rounded-xl border border-white/10 p-4"><div className="flex items-center justify-between gap-3"><div><b>{favorite.listing?.title ?? "Unavailable listing"}</b><p className="text-sm text-muted-foreground">{favorite.listing ? `${favorite.listing.price} CNY · ${favorite.listing.creator.displayName}` : "Listing was unpublished"}</p></div><Button size="sm" variant="outline" onClick={() => remove(favorite.assetId)}>Remove</Button></div></article>)}</div></div>;
}
