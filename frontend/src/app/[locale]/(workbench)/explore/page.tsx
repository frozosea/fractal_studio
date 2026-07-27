"use client";

import { useEffect, useState } from "react";
import { Heart } from "lucide-react";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { toast } from "@/components/ui/toaster";
import { platform, submitAlipayForm, type Listing } from "@/lib/api/platform";

function text(error: unknown): string {
  return error instanceof Error ? error.message : "Request failed";
}

function updateIds(ids: Set<string>, assetId: string, shouldInclude: boolean): Set<string> {
  const next = new Set(ids);
  if (shouldInclude) next.add(assetId);
  else next.delete(assetId);
  return next;
}

export default function ExplorePage() {
  const [query, setQuery] = useState("");
  const [items, setItems] = useState<Listing[]>([]);
  const [favoriteAssetIds, setFavoriteAssetIds] = useState<Set<string>>(new Set());
  const [favoriteBusy, setFavoriteBusy] = useState<Set<string>>(new Set());
  const [isLoading, setIsLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);

  const search = async () => {
    setIsLoading(true);
    setError(null);
    try {
      const value = await platform.marketplace.explore(query);
      setItems(value.data);
    } catch (reason) {
      setError(text(reason));
    } finally {
      setIsLoading(false);
    }
  };

  const loadFavorites = async () => {
    try {
      const value = await platform.marketplace.favorites();
      setFavoriteAssetIds(new Set(value.data.map((favorite) => favorite.assetId)));
    } catch (reason) {
      setError(text(reason));
    }
  };

  useEffect(() => {
    void search();
    void loadFavorites();
  }, []);

  const toggleFavorite = async (assetId: string) => {
    const wasFavorite = favoriteAssetIds.has(assetId);
    setFavoriteAssetIds((ids) => updateIds(ids, assetId, !wasFavorite));
    setFavoriteBusy((ids) => updateIds(ids, assetId, true));
    try {
      if (wasFavorite) {
        await platform.marketplace.unfavorite(assetId);
        toast({ title: "Removed from favorites", variant: "default" });
      } else {
        await platform.marketplace.favorite(assetId);
        toast({ title: "Added to favorites", description: "Saved in your Favorites library.", variant: "success" });
      }
    } catch (reason) {
      setFavoriteAssetIds((ids) => updateIds(ids, assetId, wasFavorite));
      setError(text(reason));
    } finally {
      setFavoriteBusy((ids) => updateIds(ids, assetId, false));
    }
  };

  const checkout = async (listing: Listing) => {
    try {
      submitAlipayForm((await platform.commerce.checkout(listing)).alipayForm);
    } catch (reason) {
      setError(text(reason));
    }
  };

  return (
    <div className="space-y-5">
      <div>
        <h1 className="text-2xl font-semibold">Marketplace</h1>
        <p className="text-muted-foreground">Published art. Payment state is verified by Alipay webhook.</p>
      </div>
      <div className="flex gap-2">
        <Input value={query} placeholder="Search listings" onChange={(event) => setQuery(event.target.value)} />
        <Button onClick={() => void search()} loading={isLoading}>Search</Button>
      </div>
      {error && <p className="text-red-400">{error}</p>}
      {isLoading && (
        <div className="grid grid-cols-2 gap-5" aria-label="Loading marketplace">
          {[0, 1].map((index) => <div key={index} className="aspect-[4/3] animate-pulse rounded-xl bg-white/5" />)}
        </div>
      )}
      {!isLoading && !error && items.length === 0 && (
        <p className="rounded-xl border border-dashed border-white/15 p-6 text-sm text-muted-foreground">
          No published listings found. Drafts are private: open My listings and publish a draft before it appears here.
        </p>
      )}
      {!isLoading && (
        <div className="grid grid-cols-2 gap-5">
          {items.map((listing) => {
            const isFavorite = favoriteAssetIds.has(listing.assetId);
            const isFavoriteBusy = favoriteBusy.has(listing.assetId);
            return (
              <article key={listing.id} className="min-w-0 overflow-hidden rounded-xl border border-white/10">
                <div className="aspect-[4/3] bg-white/5">
                  {listing.preview?.thumbnailUrl ? (
                    <img src={listing.preview.thumbnailUrl} alt={`Preview of ${listing.title}`} className="block h-full w-full object-cover" />
                  ) : (
                    <div className="flex h-full items-center justify-center p-3 text-center text-sm text-muted-foreground">
                      Preview unavailable
                    </div>
                  )}
                </div>
                <div className="space-y-3 p-4">
                  <div className="flex items-start justify-between gap-3">
                    <div className="min-w-0">
                      <h2 className="truncate font-medium">{listing.title}</h2>
                      <p className="text-sm text-muted-foreground">by {listing.creator.displayName} · {listing.price} CNY</p>
                    </div>
                    <Button
                      size="icon"
                      variant={isFavorite ? "neon" : "outline"}
                      disabled={isFavoriteBusy}
                      onClick={() => void toggleFavorite(listing.assetId)}
                      aria-label={isFavorite ? "Remove from favorites" : "Add to favorites"}
                      title={isFavorite ? "Remove from favorites" : "Add to favorites"}
                    >
                      <Heart className="h-4 w-4" fill={isFavorite ? "currentColor" : "none"} />
                    </Button>
                  </div>
                  {listing.description && <p className="line-clamp-2 text-sm text-muted-foreground">{listing.description}</p>}
                  <Button className="w-full" onClick={() => void checkout(listing)}>Pay with Alipay</Button>
                </div>
              </article>
            );
          })}
        </div>
      )}
    </div>
  );
}
