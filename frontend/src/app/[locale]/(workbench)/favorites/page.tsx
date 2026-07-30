"use client";

import { useEffect, useState } from "react";
import { useTranslations } from "next-intl";
import { Button } from "@/components/ui/button";
import { ListingCard } from "@/components/shared/listing-card";
import { RenderMeta } from "@/components/shared/render-meta";
import { CARD_GRID_STYLE } from "@/lib/utils/layout";
import { platform, type Listing } from "@/lib/api/platform";

type Favorite = { assetId: string; createdAt: string; listing?: Listing | null };

export default function FavoritesPage() {
  const t = useTranslations("commerce");
  const [items, setItems] = useState<Favorite[]>([]);
  const [error, setError] = useState<string | null>(null);

  const refresh = () =>
    void platform.marketplace
      .favorites()
      .then((value) => setItems(value.data))
      .catch(() => setError(t("errors.requestFailed")));
  useEffect(refresh, []);

  const remove = (assetId: string) =>
    void platform.marketplace
      .unfavorite(assetId)
      .then(refresh)
      .catch(() => setError(t("errors.requestFailed")));

  return (
    <div className="space-y-5">
      <div>
        <h1 className="text-2xl font-semibold">{t("favorites.title")}</h1>
        <p className="text-muted-foreground">{t("favorites.subtitle")}</p>
      </div>
      {error && <p className="text-red-400">{error}</p>}
      {items.length === 0 && (
        <p className="rounded-xl border border-dashed border-white/15 p-6 text-sm text-muted-foreground">
          {t("favorites.empty")}
        </p>
      )}
      <div className="grid gap-4 lg:gap-5" style={CARD_GRID_STYLE}>
        {items.map((favorite) => {
          const listing = favorite.listing;
          return (
            <ListingCard
              key={favorite.assetId}
              title={listing?.title ?? t("favorites.unavailable")}
              subtitle={listing ? `${listing.price} CNY · ${listing.creator.displayName}` : t("favorites.unpublished")}
              previewUrl={listing?.preview?.thumbnailUrl}
              previewAlt={t("marketplace.previewAlt", { title: listing?.title ?? "" })}
              previewFallback={t("marketplace.previewUnavailable")}
            >
              <RenderMeta render={listing?.render} />
              <Button
                size="sm"
                variant="outline"
                className="w-full coarse:h-11"
                onClick={() => remove(favorite.assetId)}
              >
                {t("actions.remove")}
              </Button>
            </ListingCard>
          );
        })}
      </div>
    </div>
  );
}
