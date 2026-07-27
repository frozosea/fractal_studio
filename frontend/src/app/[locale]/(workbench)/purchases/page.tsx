"use client";

import { useEffect, useState } from "react";
import { useTranslations } from "next-intl";
import { Button } from "@/components/ui/button";
import { platform, type Order } from "@/lib/api/platform";

export default function PurchasesPage() {
  const t = useTranslations("commerce");
  const [orders, setOrders] = useState<Order[]>([]); const [error, setError] = useState<string | null>(null);
  useEffect(() => { void platform.commerce.purchases().then((value) => setOrders(value.data)).catch(() => setError(t("errors.requestFailed"))); }, [t]);
  const download = async (assetId: string) => { try { window.open((await platform.assets.downloadUrl(assetId)).url, "_blank", "noopener,noreferrer"); } catch { setError(t("errors.requestFailed")); } };
  return <div className="space-y-5"><div><h1 className="text-2xl font-semibold">{t("purchases.title")}</h1><p className="text-muted-foreground">{t("purchases.subtitle")}</p></div>{error && <p className="text-red-400">{error}</p>}{orders.length === 0 && <p className="rounded-xl border border-dashed border-white/15 p-6 text-sm text-muted-foreground">{t("purchases.empty")}</p>}{orders.map((order) => <article key={order.id} className="rounded-xl border border-white/10 p-4"><div className="flex justify-between"><b>{order.amount} {order.currency}</b><span>{t(`orderStatus.${order.status}`)}</span></div><p className="mt-2 break-all text-xs text-muted-foreground">{order.id}</p>{order.status === "fulfilled" && order.items.map((item) => <Button key={item.assetId} size="sm" className="mt-2" onClick={() => void download(item.assetId)}>{t("actions.downloadAsset")}</Button>)}</article>)}</div>;
}
