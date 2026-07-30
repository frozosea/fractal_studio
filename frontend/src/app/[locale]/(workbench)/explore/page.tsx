"use client";

import { type FormEvent, useCallback, useEffect, useRef, useState } from "react";
import { Heart, Loader2, Shuffle } from "lucide-react";
import { useTranslations } from "next-intl";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { toast } from "@/components/ui/toaster";
import { ListingCard } from "@/components/shared/listing-card";
import { RenderMeta } from "@/components/shared/render-meta";
import { FacetBar, type FacetSelection } from "@/components/shared/facet-bar";
import { CARD_GRID_STYLE } from "@/lib/utils/layout";
import { platform, PlatformApiError, submitAlipayForm, type FacetCount, type Listing } from "@/lib/api/platform";

function updateIds(ids: Set<string>, assetId: string, shouldInclude: boolean): Set<string> {
  const next = new Set(ids);
  if (shouldInclude) next.add(assetId);
  else next.delete(assetId);
  return next;
}

const BATCH_SIZE = 12;
const POOL_SIZE = 48;

function shuffledListings(values: Listing[]): Listing[] {
  const result = [...values];
  for (let index = result.length - 1; index > 0; index -= 1) {
    const swapIndex = Math.floor(Math.random() * (index + 1));
    const current = result[index]!;
    result[index] = result[swapIndex]!;
    result[swapIndex] = current;
  }
  return result;
}

function uniqueListings(values: Listing[]): Listing[] {
  const seen = new Set<string>();
  return values.filter((listing) => {
    if (seen.has(listing.id)) return false;
    seen.add(listing.id);
    return true;
  });
}

export default function ExplorePage() {
  const t = useTranslations("commerce");
  const [query, setQuery] = useState("");
  const [appliedQuery, setAppliedQuery] = useState("");
  const [selection, setSelection] = useState<FacetSelection>({});
  const [facets, setFacets] = useState<FacetCount[]>([]);
  const [items, setItems] = useState<Listing[]>([]);
  const [nextCursor, setNextCursor] = useState<string | null>(null);
  const [favoriteAssetIds, setFavoriteAssetIds] = useState<Set<string>>(new Set());
  const [ownedAssetIds, setOwnedAssetIds] = useState<Set<string>>(new Set());
  const [favoriteBusy, setFavoriteBusy] = useState<Set<string>>(new Set());
  const [isLoading, setIsLoading] = useState(true);
  const [isLoadingMore, setIsLoadingMore] = useState(false);
  const [isRefreshingBatch, setIsRefreshingBatch] = useState(false);
  const [bufferedCount, setBufferedCount] = useState(0);
  const [error, setError] = useState<string | null>(null);
  const loadMoreRef = useRef<HTMLDivElement>(null);
  const loadingMoreRef = useRef(false);
  const searchRequestRef = useRef(0);
  const bufferedListingsRef = useRef<Listing[]>([]);
  const appliedSelectionRef = useRef<FacetSelection>({});

  // Every facet change goes through here rather than mutating state in place:
  // the page serves from an in-memory buffer, and a cursor is bound to the
  // filter set it was issued under, so both have to be discarded together or
  // the next page would come back 422 cursor_filter_mismatch.
  const search = useCallback(async (nextQuery: string, nextSelection: FacetSelection = {}) => {
    const requestId = ++searchRequestRef.current;
    const normalizedQuery = nextQuery.trim();
    loadingMoreRef.current = false;
    appliedSelectionRef.current = nextSelection;
    setAppliedQuery(normalizedQuery);
    setIsLoading(true);
    setIsLoadingMore(false);
    setError(null);
    try {
      const value = await platform.marketplace.explore({ q: normalizedQuery, ...nextSelection, limit: POOL_SIZE });
      if (requestId !== searchRequestRef.current) return;
      const shuffled = shuffledListings(value.data);
      const firstBatch = shuffled.slice(0, BATCH_SIZE);
      bufferedListingsRef.current = shuffled.slice(BATCH_SIZE);
      setBufferedCount(bufferedListingsRef.current.length);
      setItems(firstBatch);
      setNextCursor(value.page.nextCursor);
    } catch (reason) {
      if (requestId !== searchRequestRef.current) return;
      setError(t("errors.requestFailed"));
      setItems([]);
      bufferedListingsRef.current = [];
      setBufferedCount(0);
      setNextCursor(null);
    } finally {
      if (requestId === searchRequestRef.current) setIsLoading(false);
    }
  }, [t]);

  const loadMore = useCallback(async () => {
    if ((!nextCursor && bufferedListingsRef.current.length === 0) || loadingMoreRef.current || isLoading) return;
    const requestId = searchRequestRef.current;
    loadingMoreRef.current = true;
    setIsLoadingMore(true);
    try {
      if (bufferedListingsRef.current.length === 0 && nextCursor) {
        const value = await platform.marketplace.explore({
          q: appliedQuery, ...appliedSelectionRef.current, cursor: nextCursor, limit: POOL_SIZE,
        });
        if (requestId !== searchRequestRef.current) return;
        bufferedListingsRef.current = shuffledListings(value.data);
        setNextCursor(value.page.nextCursor);
      }
      const batch = bufferedListingsRef.current.splice(0, BATCH_SIZE);
      setBufferedCount(bufferedListingsRef.current.length);
      setItems((current) => uniqueListings([...current, ...batch]));
    } catch (reason) {
      if (requestId === searchRequestRef.current) setError(t("errors.requestFailed"));
    } finally {
      loadingMoreRef.current = false;
      if (requestId === searchRequestRef.current) setIsLoadingMore(false);
    }
  }, [appliedQuery, isLoading, nextCursor, t]);

  const refreshBatch = async () => {
    if (isLoading || isRefreshingBatch) return;
    const requestId = searchRequestRef.current;
    const currentIds = new Set(items.map((item) => item.id));
    setIsRefreshingBatch(true);
    setError(null);
    try {
      let cursor = nextCursor;
      let candidates = uniqueListings([
        ...bufferedListingsRef.current,
        ...items,
      ]);
      let alternatives = candidates.filter((item) => !currentIds.has(item.id));
      if (alternatives.length < BATCH_SIZE && cursor) {
        const value = await platform.marketplace.explore({
          q: appliedQuery, ...appliedSelectionRef.current, cursor, limit: POOL_SIZE,
        });
        if (requestId !== searchRequestRef.current) return;
        cursor = value.page.nextCursor;
        candidates = uniqueListings([...candidates, ...value.data]);
        alternatives = candidates.filter((item) => !currentIds.has(item.id));
      }

      const shuffledAlternatives = shuffledListings(alternatives);
      const nextBatch = shuffledAlternatives.slice(0, BATCH_SIZE);
      if (nextBatch.length < BATCH_SIZE) {
        const selected = new Set(nextBatch.map((item) => item.id));
        nextBatch.push(...shuffledListings(candidates)
          .filter((item) => !selected.has(item.id))
          .slice(0, BATCH_SIZE - nextBatch.length));
      }
      const selectedIds = new Set(nextBatch.map((item) => item.id));
      bufferedListingsRef.current = shuffledListings(candidates.filter((item) => !selectedIds.has(item.id)));
      setBufferedCount(bufferedListingsRef.current.length);
      setNextCursor(cursor);
      setItems(nextBatch);
      document.querySelector("main")?.scrollTo({ top: 0, behavior: "smooth" });
    } catch (reason) {
      if (requestId === searchRequestRef.current) setError(t("errors.requestFailed"));
    } finally {
      if (requestId === searchRequestRef.current) setIsRefreshingBatch(false);
    }
  };

  const loadFavorites = async () => {
    try {
      const value = await platform.marketplace.favorites();
      setFavoriteAssetIds(new Set(value.data.map((favorite) => favorite.assetId)));
    } catch (reason) {
      setError(t("errors.requestFailed"));
    }
  };

  const loadPurchases = async () => {
    try {
      const value = await platform.commerce.purchases({ fresh: true });
      setOwnedAssetIds(new Set(value.data
        .filter((order) => order.status === "fulfilled")
        .flatMap((order) => order.items.map((item) => item.assetId))));
    } catch (reason) {
      setError(t("errors.requestFailed"));
    }
  };

  const loadFacets = async () => {
    try {
      setFacets((await platform.marketplace.facets()).data);
    } catch {
      // A missing facet bar is a degraded catalogue, not a broken one — the
      // listings themselves are unaffected, so this stays quiet.
      setFacets([]);
    }
  };

  const applyFacets = (nextSelection: FacetSelection) => {
    setSelection(nextSelection);
    void search(appliedQuery, nextSelection);
  };

  useEffect(() => {
    void search("");
    void loadFacets();
    void loadFavorites();
    void loadPurchases();
  }, [search]);

  useEffect(() => {
    const target = loadMoreRef.current;
    if (!target || (!nextCursor && bufferedCount === 0) || isLoading) return;
    const observer = new IntersectionObserver(
      (entries) => {
        if (entries.some((entry) => entry.isIntersecting)) void loadMore();
      },
      { rootMargin: "480px 0px" },
    );
    observer.observe(target);
    return () => observer.disconnect();
  }, [bufferedCount, isLoading, loadMore, nextCursor]);

  const submitSearch = (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    void search(query, selection);
  };

  const toggleFavorite = async (assetId: string) => {
    const wasFavorite = favoriteAssetIds.has(assetId);
    setFavoriteAssetIds((ids) => updateIds(ids, assetId, !wasFavorite));
    setFavoriteBusy((ids) => updateIds(ids, assetId, true));
    try {
      if (wasFavorite) {
        await platform.marketplace.unfavorite(assetId);
        toast({ title: t("marketplace.removedFavorite"), variant: "default" });
      } else {
        await platform.marketplace.favorite(assetId);
        toast({ title: t("marketplace.addedFavorite"), description: t("marketplace.addedFavoriteDescription"), variant: "success" });
      }
    } catch (reason) {
      setFavoriteAssetIds((ids) => updateIds(ids, assetId, wasFavorite));
      setError(t("errors.requestFailed"));
    } finally {
      setFavoriteBusy((ids) => updateIds(ids, assetId, false));
    }
  };

  const checkout = async (listing: Listing) => {
    try {
      submitAlipayForm((await platform.commerce.checkout(listing)).alipayForm);
    } catch (reason) {
      if (reason instanceof PlatformApiError && reason.code === "asset_already_owned") {
        setOwnedAssetIds((ids) => updateIds(ids, listing.assetId, true));
        setError(t("marketplace.alreadyPurchasedDescription"));
      } else {
        setError(t("errors.requestFailed"));
      }
    }
  };

  return (
    <div className="space-y-5">
      <div>
        <h1 className="text-2xl font-semibold">{t("marketplace.title")}</h1>
        <p className="text-muted-foreground">{t("marketplace.subtitle")}</p>
      </div>
      <form className="flex gap-2" onSubmit={submitSearch}>
        <Input value={query} placeholder={t("marketplace.searchPlaceholder")} onChange={(event) => setQuery(event.target.value)} />
        <Button type="submit" loading={isLoading}>{t("actions.search")}</Button>
        <Button type="button" variant="outline" loading={isRefreshingBatch} disabled={isLoading} onClick={() => void refreshBatch()}>
          <Shuffle className="h-4 w-4" />
          <span className="hidden sm:inline">{isRefreshingBatch ? t("marketplace.refreshingBatch") : t("marketplace.refreshBatch")}</span>
        </Button>
      </form>
      <FacetBar facets={facets} selection={selection} onChange={applyFacets} disabled={isLoading} />
      {error && <p className="text-red-400">{error}</p>}
      {isLoading && (
        <div
          className="grid gap-4 lg:gap-5"
          style={CARD_GRID_STYLE}
          aria-label={t("marketplace.loading")}
        >
          {Array.from({ length: 8 }, (_, index) => <div key={index} className="aspect-[4/3] animate-pulse rounded-xl bg-white/5" />)}
        </div>
      )}
      {!isLoading && !error && items.length === 0 && (
        <p className="rounded-xl border border-dashed border-white/15 p-6 text-sm text-muted-foreground">
          {t("marketplace.empty")}
        </p>
      )}
      {!isLoading && (
        <div
          className="grid gap-4 lg:gap-5"
          style={CARD_GRID_STYLE}
        >
          {items.map((listing) => {
            const isFavorite = favoriteAssetIds.has(listing.assetId);
            const isFavoriteBusy = favoriteBusy.has(listing.assetId);
            const isOwned = ownedAssetIds.has(listing.assetId);
            return (
              <ListingCard
                key={listing.id}
                title={listing.title}
                subtitle={`${t("marketplace.byCreator", { creator: listing.creator.displayName })} · ${listing.price} CNY`}
                previewUrl={listing.preview?.thumbnailUrl}
                previewAlt={t("marketplace.previewAlt", { title: listing.title })}
                previewFallback={t("marketplace.previewUnavailable")}
                headerAction={
                  <Button
                    size="icon"
                    variant={isFavorite ? "neon" : "outline"}
                    disabled={isFavoriteBusy}
                    className="shrink-0 coarse:h-11 coarse:w-11"
                    onClick={() => void toggleFavorite(listing.assetId)}
                    aria-label={isFavorite ? t("actions.removeFavorite") : t("actions.addFavorite")}
                    title={isFavorite ? t("actions.removeFavorite") : t("actions.addFavorite")}
                  >
                    <Heart className="h-4 w-4" fill={isFavorite ? "currentColor" : "none"} />
                  </Button>
                }
              >
                <RenderMeta render={listing.render} />
                {listing.description && <p className="line-clamp-2 text-sm text-muted-foreground">{listing.description}</p>}
                <Button className="w-full coarse:h-11" disabled={isOwned} onClick={() => void checkout(listing)}>
                  {isOwned ? t("actions.alreadyPurchased") : t("actions.payAlipay")}
                </Button>
                {isOwned && <p className="text-xs text-emerald-400">{t("marketplace.alreadyPurchasedDescription")}</p>}
              </ListingCard>
            );
          })}
        </div>
      )}
      {!isLoading && items.length > 0 && (
        <div ref={loadMoreRef} className="flex min-h-12 items-center justify-center py-3 text-sm text-muted-foreground">
          {isLoadingMore ? (
            <span className="flex items-center gap-2"><Loader2 className="h-4 w-4 animate-spin" />{t("marketplace.loadingMore")}</span>
          ) : nextCursor || bufferedCount > 0 ? (
            <span className="h-1" aria-hidden="true" />
          ) : (
            <span>{t("marketplace.endOfResults")}</span>
          )}
        </div>
      )}
    </div>
  );
}
