"use client";

import { useEffect, useState } from "react";
import { useTranslations } from "next-intl";
import { Crown, Sparkles, Check, Palette, Layers, Zap } from "lucide-react";
import { Button } from "@/components/ui/button";
import { Card } from "@/components/ui/card";
import { Badge } from "@/components/ui/badge";
import { Skeleton } from "@/components/ui/skeleton";
import { toast } from "@/components/ui/toaster";
import { platform, submitAlipayForm } from "@/lib/api/platform";

export default function MembershipPage() {
  const t = useTranslations("commerce");
  const [member, setMember] = useState<boolean | null>(null);
  const [loading, setLoading] = useState(true);
  const [paying, setPaying] = useState(false);

  useEffect(() => {
    platform.membership.status()
      .then((d) => setMember(d.member))
      .catch(() => setMember(false))
      .finally(() => setLoading(false));
  }, []);

  const handlePay = async () => {
    setPaying(true);
    try {
      const result = await platform.membership.checkout();
      submitAlipayForm(result.alipayForm);
    } catch (e: unknown) {
      const err = e as { code?: string };
      if (err?.code === "already_member") {
        setMember(true);
        toast({ title: t("membership.alreadyMemberToast"), variant: "default" });
      } else if (err?.code === "payment_already_pending") {
        toast({ title: t("membership.pendingToast"), variant: "warning" });
      } else {
        toast({ title: t("errors.requestFailed"), variant: "destructive" });
      }
    } finally {
      setPaying(false);
    }
  };

  if (loading) {
    return (
      <div className="flex min-h-[60dvh] items-center justify-center">
        <div className="w-full max-w-lg space-y-6">
          <Skeleton className="mx-auto h-12 w-48" />
          <Skeleton className="mx-auto h-64 w-full rounded-2xl" />
        </div>
      </div>
    );
  }

  if (member) {
    return (
      <div className="flex min-h-[60dvh] items-center justify-center">
        <div className="w-full max-w-lg space-y-8 text-center">
          <div className="mx-auto flex h-20 w-20 items-center justify-center border border-brand/45 bg-brand/10">
            {/* Sits on the amber gradient, not on the page surface, so it
                stays white in both themes. */}
            <Crown className="h-10 w-10 text-white" />
          </div>
          <div>
            <h1 className="text-2xl font-bold">{t("membership.activeTitle")}</h1>
            <p className="mt-2 text-muted-foreground">{t("membership.activeDescription")}</p>
          </div>
          <div className="grid grid-cols-1 gap-3 text-left sm:grid-cols-2">
            {[
              { icon: Palette, text: t("membership.benefitMap") },
              { icon: Layers, text: t("membership.benefitJulia") },
              { icon: Zap, text: t("membership.benefitAll") },
              { icon: Sparkles, text: t("membership.benefitExport") },
            ].map((b, i) => (
              <div key={i} className="flex items-start gap-2 rounded-lg bg-wash/5 p-3">
                <b.icon className="mt-0.5 h-4 w-4 shrink-0 text-amber-400" />
                <span className="text-sm">{b.text}</span>
              </div>
            ))}
          </div>
        </div>
      </div>
    );
  }

  return (
    <div className="flex min-h-[60dvh] items-center justify-center">
      <div className="w-full max-w-lg space-y-8 text-center">
        {/* Hero */}
        <div className="space-y-3">
          <div className="mx-auto flex h-20 w-20 items-center justify-center rounded-full bg-wash/5 ring-1 ring-hairline/10">
            <Crown className="h-10 w-10 text-muted-foreground" />
          </div>
          <h1 className="text-2xl font-bold">{t("membership.title")}</h1>
          <p className="text-muted-foreground">{t("membership.subtitle")}</p>
        </div>

        {/* Price card */}
        <Card className="overflow-hidden border-brand/35">
          <div className="border-b border-brand/35 bg-brand/10 px-6 py-2">
            <Badge variant="fractal" className="text-xs">{t("membership.lifetime")}</Badge>
          </div>
          <div className="space-y-4 p-6">
            <div className="flex items-baseline justify-center gap-1">
              <span className="text-4xl font-bold">¥29</span>
              <span className="text-sm text-muted-foreground">CNY</span>
            </div>
            <p className="text-sm text-muted-foreground">{t("membership.oneTime")}</p>

            <ul className="space-y-2 text-left">
              {[
                t("membership.benefitMap"),
                t("membership.benefitJulia"),
                t("membership.benefitAll"),
                t("membership.benefitExport"),
              ].map((b, i) => (
                <li key={i} className="flex items-center gap-2 text-sm">
                  <Check className="h-4 w-4 shrink-0 text-emerald-400" />
                  {b}
                </li>
              ))}
            </ul>

            <Button
              className="w-full"
              variant="neon"
              size="lg"
              loading={paying}
              onClick={() => void handlePay()}
            >
              <Crown className="mr-2 h-4 w-4" />
              {t("membership.payButton")}
            </Button>
          </div>
        </Card>
      </div>
    </div>
  );
}
