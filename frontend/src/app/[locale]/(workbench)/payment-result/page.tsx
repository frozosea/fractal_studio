"use client";

import { useEffect, useState, useCallback } from "react";
import { useSearchParams } from "next/navigation";
import { useTranslations } from "next-intl";
import { CheckCircle, XCircle, Clock, Loader2, ExternalLink } from "lucide-react";
import { Link } from "@/i18n/navigation";
import { Button } from "@/components/ui/button";
import { Card } from "@/components/ui/card";
import { Badge } from "@/components/ui/badge";
import { Skeleton } from "@/components/ui/skeleton";
import { toast } from "@/components/ui/toaster";
import { platform, type Order } from "@/lib/api/platform";

type PaymentStatus =
  | "verifying"
  | "success"
  | "pending"
  | "closed"
  | "exception"
  | "not_found";

function statusConfig(status: PaymentStatus, t: ReturnType<typeof useTranslations<"commerce">>) {
  const map: Record<PaymentStatus, {
    icon: React.ReactNode;
    title: string;
    description: string;
    badge: string;
    badgeVariant: "success" | "destructive" | "warning" | "running";
  }> = {
    verifying: {
      icon: <Loader2 className="h-12 w-12 animate-spin text-neon-cyan" />,
      title: t("paymentResult.verifying.title"),
      description: t("paymentResult.verifying.description"),
      badge: t("paymentResult.verifying.badge"),
      badgeVariant: "running",
    },
    success: {
      icon: <CheckCircle className="h-12 w-12 text-green-400" />,
      title: t("paymentResult.success.title"),
      description: t("paymentResult.success.description"),
      badge: t("paymentResult.success.badge"),
      badgeVariant: "success",
    },
    pending: {
      icon: <Clock className="h-12 w-12 text-amber-400" />,
      title: t("paymentResult.pending.title"),
      description: t("paymentResult.pending.description"),
      badge: t("paymentResult.pending.badge"),
      badgeVariant: "warning",
    },
    closed: {
      icon: <XCircle className="h-12 w-12 text-gray-400" />,
      title: t("paymentResult.closed.title"),
      description: t("paymentResult.closed.description"),
      badge: t("paymentResult.closed.badge"),
      badgeVariant: "destructive",
    },
    exception: {
      icon: <XCircle className="h-12 w-12 text-red-400" />,
      title: t("paymentResult.exception.title"),
      description: t("paymentResult.exception.description"),
      badge: t("paymentResult.exception.badge"),
      badgeVariant: "destructive",
    },
    not_found: {
      icon: <XCircle className="h-12 w-12 text-gray-400" />,
      title: t("paymentResult.notFound.title"),
      description: t("paymentResult.notFound.description"),
      badge: t("paymentResult.notFound.badge"),
      badgeVariant: "destructive",
    },
  };
  return map[status];
}

function DownloadSection({ order }: { order: Order }) {
  const t = useTranslations("commerce");
  const [downloading, setDownloading] = useState<string | null>(null);

  const download = useCallback(async (assetId: string) => {
    setDownloading(assetId);
    try {
      const { url } = await platform.assets.downloadUrl(assetId);
      window.open(url, "_blank", "noopener,noreferrer");
      toast({ title: t("paymentResult.downloadStarted"), variant: "success" });
    } catch {
      toast({ title: t("errors.downloadFailed"), variant: "destructive" });
    } finally {
      setDownloading(null);
    }
  }, [t]);

  if (order.status !== "fulfilled") return null;

  return (
    <Card className="p-5 space-y-3 text-left">
      <h3 className="font-medium">{t("paymentResult.downloadAssets")}</h3>
      {order.items.map((item) => (
        <div key={item.assetId} className="flex items-center justify-between">
          <div>
            <p className="break-all text-xs text-muted-foreground font-mono">{item.assetId}</p>
            <p className="text-sm">{item.price} CNY</p>
          </div>
          <Button
            size="sm"
            variant="fractal"
            loading={downloading === item.assetId}
            onClick={() => void download(item.assetId)}
          >
            <ExternalLink className="mr-1.5 h-3.5 w-3.5" />
            {t("actions.downloadAsset")}
          </Button>
        </div>
      ))}
    </Card>
  );
}

export default function PaymentResultPage() {
  const t = useTranslations("commerce");
  const searchParams = useSearchParams();
  const [status, setStatus] = useState<PaymentStatus>("verifying");
  const [latestOrder, setLatestOrder] = useState<Order | null>(null);

  useEffect(() => {
    let cancelled = false;
    const outTradeNo = searchParams.get("out_trade_no");

    const poll = async () => {
      for (let attempt = 0; attempt < 15; attempt++) {
        if (cancelled) return;
        try {
          const page = await platform.commerce.purchases();
          if (page.data.length === 0) {
            await new Promise((r) => setTimeout(r, 2000));
            continue;
          }
          const best =
            outTradeNo
              ? page.data.find((o) => o.id === outTradeNo) ?? page.data[0]
              : page.data[0];
          if (!best || cancelled) return;
          setLatestOrder(best);
          switch (best.status) {
            case "fulfilled": setStatus("success"); return;
            case "pending_payment": break; // keep polling
            case "closed": setStatus("closed"); return;
            case "payment_exception": setStatus("exception"); return;
          }
        } catch { /* retry on network error */ }
        await new Promise((r) => setTimeout(r, 2000));
      }
      if (!cancelled) setStatus("pending");
    };

    void poll();
    return () => { cancelled = true; };
  }, [searchParams]);

  const cfg = statusConfig(status, t);
  const isMembership = status === "success" && latestOrder?.items.length === 0;

  return (
    <div className="flex min-h-[60vh] items-center justify-center">
      <div className="w-full max-w-md space-y-6 text-center">
        <div className="flex justify-center">
          {isMembership ? (
            <div className="mx-auto flex h-16 w-16 items-center justify-center rounded-full bg-gradient-to-br from-amber-400 to-orange-500 shadow-lg shadow-amber-500/20">
              <svg className="h-8 w-8 text-white" fill="none" viewBox="0 0 24 24" stroke="currentColor" strokeWidth={2}>
                <path strokeLinecap="round" strokeLinejoin="round" d="M5 3l14 9-14 9V3z" />
              </svg>
            </div>
          ) : (
            cfg.icon
          )}
        </div>
        <div>
          <h1 className="text-xl font-semibold">
            {isMembership ? t("paymentResult.membershipTitle") : cfg.title}
          </h1>
          <p className="mt-2 text-sm text-muted-foreground">
            {isMembership ? t("paymentResult.membershipDescription") : cfg.description}
          </p>
        </div>

        {status === "verifying" && (
          <div className="space-y-3">
            <Skeleton className="mx-auto h-16 w-64" />
            <Skeleton className="mx-auto h-4 w-48" />
          </div>
        )}

        {latestOrder && (
          <div className="text-left space-y-2">
            <div className="flex items-center justify-between">
              <span className="text-sm text-muted-foreground">{t("order.id")}</span>
              <Badge variant={cfg.badgeVariant}>{cfg.badge}</Badge>
            </div>
            <p className="break-all text-xs text-muted-foreground font-mono">{latestOrder.id}</p>
            <p className="text-2xl font-bold">{latestOrder.amount} {latestOrder.currency}</p>
            {latestOrder.paidAt && (
              <p className="text-xs text-muted-foreground">
                {t("order.paidAt")}: {new Date(latestOrder.paidAt).toLocaleString()}
              </p>
            )}
          </div>
        )}

        {!isMembership && latestOrder?.status === "fulfilled" && <DownloadSection order={latestOrder} />}

        {isMembership && (
          <div className="rounded-xl bg-gradient-to-b from-amber-500/10 to-transparent p-5 ring-1 ring-amber-500/20">
            <p className="text-lg font-semibold text-amber-200">{t("paymentResult.membershipWelcome")}</p>
          </div>
        )}

        <div className="flex justify-center gap-3">
          {isMembership ? (
            <Link href="/studio">
              <Button variant="neon" size="lg">{t("paymentResult.goToStudio")}</Button>
            </Link>
          ) : (
            <>
              <Link href="/purchases">
                <Button variant="outline">{t("paymentResult.viewPurchases")}</Button>
              </Link>
              <Link href="/explore">
                <Button variant="fractal">{t("paymentResult.backToExplore")}</Button>
              </Link>
            </>
          )}
        </div>
      </div>
    </div>
  );
}
