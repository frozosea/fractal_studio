"use client";

import { useEffect, useState } from "react";
import { Button } from "@/components/ui/button";
import { useAuth } from "@/providers/auth-provider";
import { useRouter } from "@/i18n/navigation";
import { platform, type InternalPayoutRequest } from "@/lib/api/platform";

function text(error: unknown): string { return error instanceof Error ? error.message : "Request failed"; }

export default function FinancePage() {
  const { user, isPending } = useAuth();
  const router = useRouter();
  const [rows, setRows] = useState<InternalPayoutRequest[]>([]); const [error, setError] = useState<string | null>(null);
  const refresh = () => void platform.finance.payouts().then((value) => setRows(value.data)).catch((reason: unknown) => setError(text(reason)));
  const allowed = Boolean(user?.roles.includes("finance_operator"));
  useEffect(() => {
    if (!isPending && !allowed) router.replace("/studio");
  }, [allowed, isPending, router]);
  useEffect(() => { if (allowed) refresh(); }, [allowed]);
  const paid = (row: InternalPayoutRequest) => {
    const externalReference = window.prompt("Alipay transfer reference");
    if (!externalReference) return;
    void platform.finance.markPaid(row.id, externalReference).then(refresh).catch((reason: unknown) => setError(text(reason)));
  };
  const reject = (row: InternalPayoutRequest) => {
    const reason = window.prompt("Reason for rejection");
    if (!reason) return;
    void platform.finance.reject(row.id, reason).then(refresh).catch((failure: unknown) => setError(text(failure)));
  };
  if (isPending || !allowed) return <div className="p-6 text-sm text-muted-foreground">Checking access…</div>;
  return <div className="space-y-5"><div><h1 className="text-2xl font-semibold">Finance payouts</h1><p className="text-muted-foreground">Pay QR request manually, then record the reference.</p></div>{error && <p className="text-red-400">{error}</p>}<div className="space-y-3">{rows.map((row) => <article key={row.id} className="rounded-xl border border-white/10 p-4"><div className="flex flex-wrap items-center justify-between gap-3"><div><b>{row.amount} {row.currency}</b> · {row.status}<p className="text-sm text-muted-foreground">{row.creator.displayName ?? row.creator.handle ?? row.creator.email ?? "Creator"}</p>{row.qrUrl && <a className="text-sm text-primary underline" href={row.qrUrl} target="_blank" rel="noreferrer">Open Alipay QR</a>}</div>{row.status === "pending" && <div className="flex gap-2"><Button size="sm" onClick={() => paid(row)}>Mark paid</Button><Button size="sm" variant="outline" onClick={() => reject(row)}>Reject</Button></div>}</div></article>)}</div></div>;
}
