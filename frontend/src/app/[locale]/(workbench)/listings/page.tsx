"use client";

import { useEffect, useState } from "react";
import { useTranslations } from "next-intl";
import { Button } from "@/components/ui/button";
import { platform, PlatformApiError, type Listing } from "@/lib/api/platform";
import { Link } from "@/i18n/navigation";

export default function ListingsPage() {
  const t = useTranslations("commerce");
  const tNav = useTranslations("workbench");
  const [listings, setListings] = useState<Listing[]>([]); const [error, setError] = useState<string | null>(null);
  // Listings need the creator role; an account without one is not broken, it
  // just has not applied for a creator profile yet.
  const [needsCreatorProfile, setNeedsCreatorProfile] = useState(false);
  // Only the listing GET reads 403 this way: it carries no CSRF token, so the
  // role check is the sole thing that can forbid it. The mutations below can
  // also fail CSRF, which is not a missing creator profile.
  const fail = (reason: unknown) => {
    if (reason instanceof PlatformApiError && reason.status === 403) setNeedsCreatorProfile(true);
    else setError(t("errors.requestFailed"));
  };
  const refresh = () => void platform.marketplace.mine().then((value) => { setNeedsCreatorProfile(false); setListings(value.data); }).catch(fail);
  useEffect(refresh, []);
  const change = (listing: Listing) => void (listing.status === "published" ? platform.marketplace.unpublish(listing.id) : platform.marketplace.publish(listing.id)).then(refresh).catch(() => setError(t("errors.requestFailed")));
  const edit = (listing: Listing) => {
    const title = window.prompt(t("listings.titlePrompt"), listing.title);
    if (!title || title === listing.title) return;
    void platform.marketplace.update(listing.id, { title }).then(refresh).catch(() => setError(t("errors.requestFailed")));
  };
  return <div className="space-y-5"><div><h1 className="text-2xl font-semibold">{t("listings.title")}</h1><p className="text-muted-foreground">{t("listings.subtitle")}</p></div>{error && <p className="text-red-400">{error}</p>}{needsCreatorProfile && <p className="rounded-xl border border-dashed border-white/15 p-6 text-sm text-muted-foreground">{t("errors.creatorRequired")} <Link href="/payouts" className="text-amber-400 underline underline-offset-4">{tNav("nav.payouts")}</Link></p>}{!needsCreatorProfile && listings.length === 0 && <p className="rounded-xl border border-dashed border-white/15 p-6 text-sm text-muted-foreground">{t("listings.empty")}</p>}<div className="grid grid-cols-2 gap-5">{listings.map((listing) => <article key={listing.id} className="min-w-0 overflow-hidden rounded-xl border border-white/10"><div className="aspect-[4/3] bg-white/5">{listing.preview?.thumbnailUrl ? <img src={listing.preview.thumbnailUrl} alt={t("marketplace.previewAlt", { title: listing.title })} className="block h-full w-full object-cover" /> : <div className="flex h-full items-center justify-center p-3 text-center text-sm text-muted-foreground">{t("listings.previewPreparing")}</div>}</div><div className="p-4"><div className="flex flex-wrap items-center justify-between gap-2"><div className="min-w-0"><b className="block truncate">{listing.title}</b><p className="text-sm text-muted-foreground">{listing.price} CNY · {t(`listingStatus.${listing.status}`)}</p></div><div className="flex gap-2"><Button size="sm" variant="outline" onClick={() => edit(listing)}>{t("actions.editTitle")}</Button><Button size="sm" disabled={listing.status !== "published" && !listing.preview?.watermarkedPreviewUrl} onClick={() => change(listing)}>{listing.status === "published" ? t("actions.unpublish") : t("actions.publish")}</Button></div></div><p className="mt-2 text-sm">{listing.description || t("listings.noDescription")}</p>{listing.status !== "published" && !listing.preview?.watermarkedPreviewUrl && <p className="mt-2 text-xs text-muted-foreground">{t("listings.publicPreviewPreparing")}</p>}</div></article>)}</div></div>;
}
