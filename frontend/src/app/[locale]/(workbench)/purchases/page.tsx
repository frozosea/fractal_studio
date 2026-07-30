"use client";

import { useEffect, useState } from "react";
import { useTranslations } from "next-intl";
import { Download } from "lucide-react";
import { Button } from "@/components/ui/button";
import { ListingCard } from "@/components/shared/listing-card";
import { RenderMeta } from "@/components/shared/render-meta";
import { CARD_GRID_STYLE } from "@/lib/utils/layout";
import { platform, type Order, type RenderMeta as RenderMetaValue } from "@/lib/api/platform";

/**
 * The published snapshot recorded on the order item, frozen at purchase time so
 * a buyer keeps seeing what they actually bought even if the listing is later
 * edited or withdrawn.
 */
type Snapshot = {
  title?: string;
  creator?: { handle?: string; displayName?: string };
  render?: RenderMetaValue | null;
};

export default function PurchasesPage() {
  const t = useTranslations("commerce");
  const [orders, setOrders] = useState<Order[]>([]);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    void platform.commerce
      .purchases()
      .then((value) => setOrders(value.data))
      .catch(() => setError(t("errors.requestFailed")));
  }, [t]);

  const download = async (assetId: string) => {
    try {
      window.open((await platform.assets.downloadUrl(assetId)).url, "_blank", "noopener,noreferrer");
    } catch {
      setError(t("errors.requestFailed"));
    }
  };

  return (
    <div className="space-y-6">
      <div>
        <h1 className="text-2xl font-semibold">{t("purchases.title")}</h1>
        <p className="text-muted-foreground">{t("purchases.subtitle")}</p>
      </div>
      {error && <p className="text-red-400">{error}</p>}
      {orders.length === 0 && (
        <p className="rounded-xl border border-dashed border-white/15 p-6 text-sm text-muted-foreground">
          {t("purchases.empty")}
        </p>
      )}

      {orders.map((order) => (
        <section key={order.id} className="space-y-3">
          <div className="flex flex-wrap items-baseline justify-between gap-x-3 gap-y-1">
            <b>
              {order.amount} {order.currency}
            </b>
            <span className="text-sm text-muted-foreground">{t(`orderStatus.${order.status}`)}</span>
            <p className="w-full break-all font-mono text-xs text-muted-foreground">{order.id}</p>
          </div>

          <div className="grid gap-4 lg:gap-5" style={CARD_GRID_STYLE}>
            {order.items.map((item) => {
              const snapshot = (item.snapshot ?? {}) as Snapshot;
              const title = snapshot.title ?? t("purchases.untitled");
              return (
                <ListingCard
                  key={item.assetId}
                  title={title}
                  subtitle={
                    snapshot.creator?.displayName
                      ? `${t("marketplace.byCreator", { creator: snapshot.creator.displayName })} · ${item.price} CNY`
                      : `${item.price} CNY`
                  }
                  // A purchased master is private, so there is no public
                  // thumbnail to show here — the download is the artwork.
                  previewUrl={null}
                  previewAlt={t("marketplace.previewAlt", { title })}
                  previewFallback={t("purchases.downloadToView")}
                >
                  <RenderMeta render={snapshot.render} />
                  {order.status === "fulfilled" && (
                    <Button className="w-full coarse:h-11" onClick={() => void download(item.assetId)}>
                      <Download className="h-4 w-4" />
                      {t("actions.downloadAsset")}
                    </Button>
                  )}
                </ListingCard>
              );
            })}
          </div>
        </section>
      ))}
    </div>
  );
}
