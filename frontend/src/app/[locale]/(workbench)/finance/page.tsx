"use client";

import { useEffect, useState } from "react";
import { useTranslations } from "next-intl";
import { Button } from "@/components/ui/button";
import { useAuth } from "@/providers/auth-provider";
import { useRouter } from "@/i18n/navigation";
import { platform, type InternalPayoutRequest } from "@/lib/api/platform";

export default function FinancePage() {
  const t = useTranslations("commerce");
  const { user, isPending } = useAuth();
  const router = useRouter();
  const [rows, setRows] = useState<InternalPayoutRequest[]>([]); const [error, setError] = useState<string | null>(null);
  const refresh = () => void platform.finance.payouts().then((value) => setRows(value.data)).catch(() => setError(t("errors.requestFailed")));
  const allowed = Boolean(user?.roles.includes("finance_operator"));
  useEffect(() => {
    if (!isPending && !allowed) router.replace("/studio");
  }, [allowed, isPending, router]);
  useEffect(() => { if (allowed) refresh(); }, [allowed]);
  const paid = (row: InternalPayoutRequest) => {
    const externalReference = window.prompt(t("finance.transferReferencePrompt"));
    if (!externalReference) return;
    void platform.finance.markPaid(row.id, externalReference).then(refresh).catch(() => setError(t("errors.requestFailed")));
  };
  const reject = (row: InternalPayoutRequest) => {
    const reason = window.prompt(t("finance.rejectionPrompt"));
    if (!reason) return;
    void platform.finance.reject(row.id, reason).then(refresh).catch(() => setError(t("errors.requestFailed")));
  };
  if (isPending || !allowed) return <div className="p-6 text-sm text-muted-foreground">{t("finance.checkingAccess")}</div>;
  return <div className="space-y-5"><div><h1 className="text-2xl font-semibold">{t("finance.title")}</h1><p className="text-muted-foreground">{t("finance.subtitle")}</p></div>{error && <p className="text-red-400">{error}</p>}{rows.length === 0 && <p className="rounded-xl border border-dashed border-white/15 p-6 text-sm text-muted-foreground">{t("finance.empty")}</p>}<div className="space-y-3">{rows.map((row) => <article key={row.id} className="rounded-xl border border-white/10 p-4"><div className="flex flex-wrap items-center justify-between gap-3"><div><b>{row.amount} {row.currency}</b> · {t(`payoutStatus.${row.status}`)}<p className="text-sm text-muted-foreground">{row.creator.displayName ?? row.creator.handle ?? row.creator.email ?? t("finance.creator")}</p>{row.qrUrl && <a className="text-sm text-primary underline" href={row.qrUrl} target="_blank" rel="noreferrer">{t("finance.openQr")}</a>}</div>{row.status === "pending" && <div className="flex gap-2"><Button size="sm" onClick={() => paid(row)}>{t("finance.markPaid")}</Button><Button size="sm" variant="outline" onClick={() => reject(row)}>{t("finance.reject")}</Button></div>}</div></article>)}</div></div>;
}
