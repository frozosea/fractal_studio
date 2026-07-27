"use client";

import { useEffect, useState } from "react";
import { Button } from "@/components/ui/button";
import { platform, type Order } from "@/lib/api/platform";

function text(error: unknown): string { return error instanceof Error ? error.message : "Request failed"; }

export default function PurchasesPage() {
  const [orders, setOrders] = useState<Order[]>([]); const [error, setError] = useState<string | null>(null);
  useEffect(() => { void platform.commerce.purchases().then((value) => setOrders(value.data)).catch((reason: unknown) => setError(text(reason))); }, []);
  const download = async (assetId: string) => { try { window.open((await platform.assets.downloadUrl(assetId)).url, "_blank", "noopener,noreferrer"); } catch (reason) { setError(text(reason)); } };
  return <div className="space-y-5"><div><h1 className="text-2xl font-semibold">Purchases</h1><p className="text-muted-foreground">Downloads available only for active entitlement.</p></div>{error && <p className="text-red-400">{error}</p>}{orders.map((order) => <article key={order.id} className="rounded-xl border border-white/10 p-4"><div className="flex justify-between"><b>{order.amount} {order.currency}</b><span>{order.status}</span></div><p className="mt-2 break-all text-xs text-muted-foreground">{order.id}</p>{order.status === "fulfilled" && order.items.map((item) => <Button key={item.assetId} size="sm" className="mt-2" onClick={() => void download(item.assetId)}>Download asset</Button>)}</article>)}</div>;
}
