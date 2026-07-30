"use client";

import { useEffect, useState } from "react";
import { useTranslations } from "next-intl";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { Dialog, DialogContent, DialogDescription, DialogFooter, DialogHeader, DialogTitle } from "@/components/ui/dialog";
import { useAuth } from "@/providers/auth-provider";
import { useRouter } from "@/i18n/navigation";
import { platform, type InternalPayoutRequest } from "@/lib/api/platform";

/** Which text the operator is being asked for; also drives the dialog copy. */
type PromptKind = "paid" | "reject";

export default function FinancePage() {
  const t = useTranslations("commerce");
  const tCommon = useTranslations("common");
  const { user, isPending } = useAuth();
  const router = useRouter();
  const [rows, setRows] = useState<InternalPayoutRequest[]>([]);
  const [error, setError] = useState<string | null>(null);
  const [prompt, setPrompt] = useState<{ kind: PromptKind; row: InternalPayoutRequest } | null>(null);
  const [value, setValue] = useState("");
  const [submitting, setSubmitting] = useState(false);

  const refresh = () =>
    void platform.finance
      .payouts()
      .then((result) => setRows(result.data))
      .catch(() => setError(t("errors.requestFailed")));
  const allowed = Boolean(user?.roles.includes("finance_operator"));

  useEffect(() => {
    if (!isPending && !allowed) router.replace("/studio");
  }, [allowed, isPending, router]);
  useEffect(() => {
    if (allowed) refresh();
  }, [allowed]);

  const open = (kind: PromptKind, row: InternalPayoutRequest) => {
    setValue("");
    setError(null);
    setPrompt({ kind, row });
  };

  const submit = async () => {
    if (!prompt) return;
    const text = value.trim();
    if (!text) return;
    setSubmitting(true);
    try {
      await (prompt.kind === "paid"
        ? platform.finance.markPaid(prompt.row.id, text)
        : platform.finance.reject(prompt.row.id, text));
      setPrompt(null);
      refresh();
    } catch {
      setError(t("errors.requestFailed"));
    } finally {
      setSubmitting(false);
    }
  };

  if (isPending || !allowed) {
    return <div className="p-6 text-sm text-muted-foreground">{t("finance.checkingAccess")}</div>;
  }

  return (
    <div className="space-y-5">
      <div>
        <h1 className="text-2xl font-semibold">{t("finance.title")}</h1>
        <p className="text-muted-foreground">{t("finance.subtitle")}</p>
      </div>
      {error && <p className="text-red-400">{error}</p>}
      {rows.length === 0 && (
        <p className="rounded-xl border border-dashed border-white/15 p-6 text-sm text-muted-foreground">
          {t("finance.empty")}
        </p>
      )}

      <div className="space-y-3">
        {rows.map((row) => (
          <article key={row.id} className="rounded-xl border border-white/10 p-4">
            {/* Stacks under `sm`: an amount, a status and two buttons do not fit
                on one line at phone width. */}
            <div className="flex flex-col gap-3 sm:flex-row sm:flex-wrap sm:items-center sm:justify-between">
              <div className="min-w-0">
                <b>
                  {row.amount} {row.currency}
                </b>{" "}
                · {t(`payoutStatus.${row.status}`)}
                <p className="truncate text-sm text-muted-foreground">
                  {row.creator.displayName ?? row.creator.handle ?? row.creator.email ?? t("finance.creator")}
                </p>
                {row.qrUrl && (
                  <a className="text-sm text-primary underline" href={row.qrUrl} target="_blank" rel="noreferrer">
                    {t("finance.openQr")}
                  </a>
                )}
              </div>
              {row.status === "pending" && (
                <div className="flex flex-wrap gap-2">
                  <Button size="sm" className="coarse:h-11" onClick={() => open("paid", row)}>
                    {t("finance.markPaid")}
                  </Button>
                  <Button size="sm" variant="outline" className="coarse:h-11" onClick={() => open("reject", row)}>
                    {t("finance.reject")}
                  </Button>
                </div>
              )}
            </div>
          </article>
        ))}
      </div>

      {/* A real dialog rather than window.prompt: the native prompt is unstyled,
          uncancellable by tapping away, and unusable on several mobile browsers. */}
      <Dialog
        open={Boolean(prompt)}
        onOpenChange={(next) => {
          if (!next && !submitting) setPrompt(null);
        }}
      >
        <DialogContent>
          <DialogHeader>
            <DialogTitle>
              {prompt?.kind === "reject" ? t("finance.reject") : t("finance.markPaid")}
            </DialogTitle>
            <DialogDescription>
              {prompt?.kind === "reject" ? t("finance.rejectionPrompt") : t("finance.transferReferencePrompt")}
            </DialogDescription>
          </DialogHeader>
          <Input
            autoFocus
            value={value}
            maxLength={500}
            onChange={(event) => setValue(event.target.value)}
            onKeyDown={(event) => {
              if (event.key === "Enter" && value.trim() && !submitting) void submit();
            }}
          />
          <DialogFooter>
            <Button variant="outline" disabled={submitting} onClick={() => setPrompt(null)}>
              {t("actions.cancel")}
            </Button>
            <Button loading={submitting} disabled={!value.trim()} onClick={() => void submit()}>
              {tCommon("confirm")}
            </Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>
    </div>
  );
}
