"use client";

import { useCallback, useEffect, useState } from "react";
import { useTranslations } from "next-intl";
import { Loader2 } from "lucide-react";
import { Button } from "@/components/ui/button";
import { ListingCard } from "@/components/shared/listing-card";
import { RenderMeta } from "@/components/shared/render-meta";
import { CARD_GRID_STYLE } from "@/lib/utils/layout";
import { platform, type Listing } from "@/lib/api/platform";

/**
 * A creator's published work, newest first.
 *
 * Deliberately does not reuse the marketplace page's data path: that one
 * shuffles a 48-item pool client-side and fires authenticated favourites and
 * purchases requests on mount, both of which are wrong here — a profile should
 * be stable between visits and has to work for a signed-out visitor.
 */
export function CreatorGallery({ handle }: { handle: string }) {
  const t = useTranslations("commerce");
  const tCreator = useTranslations("creator");
  const [items, setItems] = useState<Listing[]>([]);
  const [cursor, setCursor] = useState<string | null>(null);
  const [isLoading, setIsLoading] = useState(true);
  const [isLoadingMore, setIsLoadingMore] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const load = useCallback(
    async (nextCursor: string | null) => {
      try {
        const value = await platform.marketplace.creatorListings(handle, nextCursor);
        setItems((current) => (nextCursor ? [...current, ...value.data] : value.data));
        setCursor(value.page.nextCursor);
      } catch {
        setError(t("errors.requestFailed"));
      } finally {
        setIsLoading(false);
        setIsLoadingMore(false);
      }
    },
    [handle, t],
  );

  useEffect(() => {
    void load(null);
  }, [load]);

  return (
    <div className="mt-8 space-y-5">
      {error && <p className="text-red-400">{error}</p>}

      {isLoading && (
        <div className="grid gap-4 lg:gap-5" style={CARD_GRID_STYLE} aria-label={t("marketplace.loading")}>
          {Array.from({ length: 6 }, (_, index) => (
            <div key={index} className="aspect-[4/3] animate-pulse rounded-xl bg-white/5" />
          ))}
        </div>
      )}

      {!isLoading && items.length === 0 && !error && (
        <p className="rounded-xl border border-dashed border-white/15 p-6 text-sm text-muted-foreground">
          {tCreator("empty")}
        </p>
      )}

      {items.length > 0 && (
        <div className="grid gap-4 lg:gap-5" style={CARD_GRID_STYLE}>
          {items.map((listing) => (
            <ListingCard
              key={listing.id}
              title={listing.title}
              subtitle={`${listing.price} CNY`}
              previewUrl={listing.preview?.thumbnailUrl}
              previewAlt={t("marketplace.previewAlt", { title: listing.title })}
              previewFallback={t("marketplace.previewUnavailable")}
            >
              <RenderMeta render={listing.render} />
              {listing.description && (
                <p className="line-clamp-2 text-sm text-muted-foreground">{listing.description}</p>
              )}
            </ListingCard>
          ))}
        </div>
      )}

      {cursor && (
        <Button
          variant="outline"
          className="w-full coarse:h-12"
          loading={isLoadingMore}
          onClick={() => {
            setIsLoadingMore(true);
            void load(cursor);
          }}
        >
          {isLoadingMore ? <Loader2 className="h-4 w-4 animate-spin" /> : null}
          {tCreator("loadMore")}
        </Button>
      )}
    </div>
  );
}
