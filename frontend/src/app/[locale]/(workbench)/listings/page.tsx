"use client";

import { useEffect, useState } from "react";
import { Button } from "@/components/ui/button";
import { platform, type Listing } from "@/lib/api/platform";

function text(error: unknown): string { return error instanceof Error ? error.message : "Request failed"; }

export default function ListingsPage() {
  const [listings, setListings] = useState<Listing[]>([]); const [error, setError] = useState<string | null>(null);
  const refresh = () => void platform.marketplace.mine().then((value) => setListings(value.data)).catch((reason: unknown) => setError(text(reason)));
  useEffect(refresh, []);
  const change = (listing: Listing) => void (listing.status === "published" ? platform.marketplace.unpublish(listing.id) : platform.marketplace.publish(listing.id)).then(refresh).catch((reason: unknown) => setError(text(reason)));
  const edit = (listing: Listing) => {
    const title = window.prompt("Listing title", listing.title);
    if (!title || title === listing.title) return;
    void platform.marketplace.update(listing.id, { title }).then(refresh).catch((reason: unknown) => setError(text(reason)));
  };
  return <div className="space-y-5"><div><h1 className="text-2xl font-semibold">My listings</h1><p className="text-muted-foreground">Draft from Library, edit offer, then publish.</p></div>{error && <p className="text-red-400">{error}</p>}<div className="grid grid-cols-2 gap-5">{listings.map((listing) => <article key={listing.id} className="min-w-0 overflow-hidden rounded-xl border border-white/10"><div className="aspect-[4/3] bg-white/5">{listing.preview?.thumbnailUrl ? <img src={listing.preview.thumbnailUrl} alt={`Preview of ${listing.title}`} className="block h-full w-full object-cover" /> : <div className="flex h-full items-center justify-center p-3 text-center text-sm text-muted-foreground">Preview is being prepared</div>}</div><div className="p-4"><div className="flex flex-wrap items-center justify-between gap-2"><div className="min-w-0"><b className="block truncate">{listing.title}</b><p className="text-sm text-muted-foreground">{listing.price} CNY · {listing.status}</p></div><div className="flex gap-2"><Button size="sm" variant="outline" onClick={() => edit(listing)}>Edit title</Button><Button size="sm" disabled={listing.status !== "published" && !listing.preview?.watermarkedPreviewUrl} onClick={() => change(listing)}>{listing.status === "published" ? "Unpublish" : "Publish"}</Button></div></div><p className="mt-2 text-sm">{listing.description || "No description"}</p>{listing.status !== "published" && !listing.preview?.watermarkedPreviewUrl && <p className="mt-2 text-xs text-muted-foreground">Public preview is still being prepared.</p>}</div></article>)}</div></div>;
}
