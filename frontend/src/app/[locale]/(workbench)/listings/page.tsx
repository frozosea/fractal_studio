"use client";

import { useEffect, useState } from "react";
import { useTranslations } from "next-intl";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { Dialog, DialogContent, DialogDescription, DialogFooter, DialogHeader, DialogTitle } from "@/components/ui/dialog";
import { ListingCard } from "@/components/shared/listing-card";
import { RenderMeta } from "@/components/shared/render-meta";
import { CARD_GRID_STYLE } from "@/lib/utils/layout";
import { platform, PlatformApiError, type Listing } from "@/lib/api/platform";
import { Link } from "@/i18n/navigation";

type Draft = { title: string; description: string; price: string };

export default function ListingsPage() {
  const t = useTranslations("commerce");
  const tNav = useTranslations("workbench");
  const tCommon = useTranslations("common");
  const [listings, setListings] = useState<Listing[]>([]);
  const [error, setError] = useState<string | null>(null);
  // Listings need the creator role; an account without one is not broken, it
  // just has not applied for a creator profile yet.
  const [needsCreatorProfile, setNeedsCreatorProfile] = useState(false);
  const [editing, setEditing] = useState<Listing | null>(null);
  const [draft, setDraft] = useState<Draft>({ title: "", description: "", price: "" });
  const [saving, setSaving] = useState(false);
  const [editError, setEditError] = useState<string | null>(null);

  // Only the listing GET reads 403 this way: it carries no CSRF token, so the
  // role check is the sole thing that can forbid it. The mutations below can
  // also fail CSRF, which is not a missing creator profile.
  const fail = (reason: unknown) => {
    if (reason instanceof PlatformApiError && reason.status === 403) setNeedsCreatorProfile(true);
    else setError(t("errors.requestFailed"));
  };
  const refresh = () =>
    void platform.marketplace
      .mine()
      .then((value) => {
        setNeedsCreatorProfile(false);
        setListings(value.data);
      })
      .catch(fail);
  useEffect(refresh, []);

  const change = (listing: Listing) =>
    void (listing.status === "published"
      ? platform.marketplace.unpublish(listing.id)
      : platform.marketplace.publish(listing.id)
    )
      .then(refresh)
      .catch(() => setError(t("errors.requestFailed")));

  const openEdit = (listing: Listing) => {
    setDraft({ title: listing.title, description: listing.description, price: listing.price });
    setEditError(null);
    setEditing(listing);
  };

  const saveEdit = async () => {
    if (!editing) return;
    setSaving(true);
    setEditError(null);
    try {
      // Send only what actually changed, so an untouched field never overwrites
      // a value edited elsewhere.
      const patch: Partial<Draft> = {};
      if (draft.title.trim() !== editing.title) patch.title = draft.title.trim();
      if (draft.description !== editing.description) patch.description = draft.description;
      if (draft.price !== editing.price) patch.price = draft.price;
      if (Object.keys(patch).length > 0) await platform.marketplace.update(editing.id, patch);
      setEditing(null);
      refresh();
    } catch {
      setEditError(t("errors.requestFailed"));
    } finally {
      setSaving(false);
    }
  };

  return (
    <div className="space-y-5">
      <div>
        <h1 className="text-2xl font-semibold">{t("listings.title")}</h1>
        <p className="text-muted-foreground">{t("listings.subtitle")}</p>
      </div>
      {error && <p className="text-red-400">{error}</p>}
      {needsCreatorProfile && (
        <p className="rounded-xl border border-dashed border-hairline/15 p-6 text-sm text-muted-foreground">
          {t("errors.creatorRequired")}{" "}
          <Link href="/payouts" className="text-amber-400 underline underline-offset-4">
            {tNav("nav.payouts")}
          </Link>
        </p>
      )}
      {!needsCreatorProfile && listings.length === 0 && (
        <p className="rounded-xl border border-dashed border-hairline/15 p-6 text-sm text-muted-foreground">
          {t("listings.empty")}
        </p>
      )}

      <div className="grid gap-4 lg:gap-5" style={CARD_GRID_STYLE}>
        {listings.map((listing) => (
          <ListingCard
            key={listing.id}
            title={listing.title}
            subtitle={`${listing.price} CNY · ${t(`listingStatus.${listing.status}`)}`}
            previewUrl={listing.preview?.thumbnailUrl}
            previewAlt={t("marketplace.previewAlt", { title: listing.title })}
            previewFallback={t("listings.previewPreparing")}
          >
            <RenderMeta render={listing.render} />
            <p className="line-clamp-2 text-sm text-muted-foreground">
              {listing.description || t("listings.noDescription")}
            </p>
            {/* Wraps rather than sitting in a fixed row: two buttons plus a long
                status label do not fit side by side on a narrow card. */}
            <div className="flex flex-wrap gap-2">
              <Button size="sm" variant="outline" className="flex-1 coarse:h-11" onClick={() => openEdit(listing)}>
                {tCommon("edit")}
              </Button>
              <Button
                size="sm"
                className="flex-1 coarse:h-11"
                disabled={listing.status !== "published" && !listing.preview?.watermarkedPreviewUrl}
                onClick={() => change(listing)}
              >
                {listing.status === "published" ? t("actions.unpublish") : t("actions.publish")}
              </Button>
            </div>
            {listing.status !== "published" && !listing.preview?.watermarkedPreviewUrl && (
              <p className="text-xs text-muted-foreground">{t("listings.publicPreviewPreparing")}</p>
            )}
          </ListingCard>
        ))}
      </div>

      <Dialog
        open={Boolean(editing)}
        onOpenChange={(open) => {
          if (!open && !saving) setEditing(null);
        }}
      >
        <DialogContent>
          <DialogHeader>
            <DialogTitle>{t("listings.editTitle")}</DialogTitle>
            <DialogDescription>{t("listings.editHint")}</DialogDescription>
          </DialogHeader>
          <div className="space-y-3">
            <Input
              autoFocus
              placeholder={t("assets.listingTitle")}
              value={draft.title}
              maxLength={120}
              onChange={(event) => setDraft((current) => ({ ...current, title: event.target.value }))}
            />
            <textarea
              className="min-h-24 w-full rounded-lg border border-hairline/[0.08] bg-transparent p-3 text-sm text-ink/80 outline-none placeholder:text-ink/30 focus-visible:border-hairline/20"
              placeholder={t("listings.descriptionPlaceholder")}
              value={draft.description}
              maxLength={4000}
              onChange={(event) => setDraft((current) => ({ ...current, description: event.target.value }))}
            />
            <Input
              placeholder={t("assets.priceCny")}
              type="number"
              min="0.01"
              step="0.01"
              value={draft.price}
              onChange={(event) => setDraft((current) => ({ ...current, price: event.target.value }))}
            />
            {editError && <p className="text-sm text-red-400">{editError}</p>}
          </div>
          <DialogFooter>
            <Button variant="outline" disabled={saving} onClick={() => setEditing(null)}>
              {t("actions.cancel")}
            </Button>
            <Button loading={saving} disabled={!draft.title.trim() || !draft.price} onClick={() => void saveEdit()}>
              {tCommon("save")}
            </Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>
    </div>
  );
}
