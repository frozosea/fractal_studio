"use client";

import { useEffect, useState } from "react";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { toast } from "@/components/ui/toaster";
import { useRouter } from "@/i18n/navigation";
import { platform, PlatformApiError, type Asset } from "@/lib/api/platform";

function errorText(error: unknown): string {
  if (error instanceof PlatformApiError && error.status === 403) {
    return "Create a creator profile in Payouts before publishing a listing.";
  }
  return error instanceof Error ? error.message : "Request failed";
}

export default function AssetsPage() {
  const router = useRouter();
  const [assets, setAssets] = useState<Asset[]>([]);
  const [listingAsset, setListingAsset] = useState<string | null>(null);
  const [title, setTitle] = useState("");
  const [price, setPrice] = useState("19.90");
  const [error, setError] = useState<string | null>(null);

  const refresh = () => void platform.assets.list().then((value) => setAssets(value.data)).catch((reason: unknown) => setError(errorText(reason)));
  useEffect(refresh, []);

  const download = async (assetId: string) => {
    try { window.open((await platform.assets.downloadUrl(assetId)).url, "_blank", "noopener,noreferrer"); }
    catch (reason) { setError(errorText(reason)); }
  };

  const createListing = async () => {
    if (!listingAsset) return;
    try {
      const listing = await platform.marketplace.create({ assetId: listingAsset, title, description: "", tags: ["fractal"], price, licenceOffer: { code: "personal", termsVersion: "v1" } });
      toast({
        title: "Draft listing created",
        description: `Publish “${listing.title}” in My listings to make it visible in Marketplace.`,
        variant: "success",
      });
      router.push("/listings");
    } catch (reason) { setError(errorText(reason)); }
  };

  return <div className="space-y-5">
    <div><h1 className="text-2xl font-semibold">My library</h1><p className="text-muted-foreground">Private assets become sellable listings only through Platform.</p></div>
    {error && <p className="text-red-400">{error}</p>}
    {assets.length === 0 && <p className="rounded border border-dashed p-6 text-muted-foreground">No assets yet. Render a recipe in Studio.</p>}
    <div className="grid gap-3 md:grid-cols-2">{assets.map((asset) => <article key={asset.id} className="rounded-xl border border-white/10 p-4 text-sm">
      <div className="flex justify-between"><b>{asset.mediaType}</b><span>{asset.status}</span></div><p className="mt-2 break-all text-muted-foreground">{asset.id}</p><p>{asset.visibility} · derivatives: {asset.derivativeStatus}</p>
      <div className="mt-3 flex flex-wrap gap-2">
        {asset.status === "ready" && <Button size="sm" onClick={() => void download(asset.id)}>Download</Button>}
        {asset.status === "ready" && asset.derivativeStatus === "ready" && <Button size="sm" variant="outline" onClick={() => setListingAsset(asset.id)}>Create listing</Button>}
        {asset.status === "ready" && asset.derivativeStatus === "pending" && <span className="self-center text-xs text-muted-foreground">Preparing public preview…</span>}
        <Button size="sm" variant="outline" onClick={() => void platform.assets.setVisibility(asset.id, asset.visibility === "private" ? "hidden" : "private").then(refresh).catch((reason: unknown) => setError(errorText(reason)))}>{asset.visibility === "private" ? "Hide" : "Restore"}</Button>
        <Button size="sm" variant="outline" onClick={() => void platform.assets.remove(asset.id).then(refresh).catch((reason: unknown) => setError(errorText(reason)))}>Delete</Button>
      </div>
    </article>)}</div>
    {listingAsset && <section className="max-w-lg rounded-xl border border-primary/30 p-4"><h2 className="mb-3 text-lg font-medium">Create draft listing</h2><p className="mb-3 text-sm text-muted-foreground">Drafts are private until you publish them from My listings.</p><Input placeholder="Title" value={title} onChange={(event) => setTitle(event.target.value)} /><Input className="mt-2" placeholder="CNY price" value={price} onChange={(event) => setPrice(event.target.value)} /><div className="mt-3 flex gap-2"><Button onClick={() => void createListing()} disabled={!title}>Create draft</Button><Button variant="outline" onClick={() => setListingAsset(null)}>Cancel</Button></div></section>}
  </div>;
}
