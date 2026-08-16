"use client";

import { useEffect, useRef, useState } from "react";
import { Check, Sparkles, Square } from "lucide-react";
import { useTranslations } from "next-intl";
import { Badge } from "@/components/ui/badge";
import { Button } from "@/components/ui/button";
import {
  platform,
  PlatformApiError,
  type AIAllowance,
  type AIListingCopyCandidate,
} from "@/lib/api/platform";
import { cn } from "@/lib/utils/cn";

interface AIListingCopyProps {
  listingId: string;
  locale: string;
  onApply: (candidate: AIListingCopyCandidate) => void;
  onBusyChange?: (busy: boolean) => void;
}

interface ListingCopyBody {
  listingId: string;
  locale: string;
  sourceRequestId?: string;
  instruction?: string;
}

interface PendingAttempt {
  body: ListingCopyBody;
  key: string;
}

export function AIListingCopy({
  listingId,
  locale,
  onApply,
  onBusyChange,
}: AIListingCopyProps) {
  const t = useTranslations("commerce.listings.aiCopy");
  const [allowance, setAllowance] = useState<AIAllowance | null>(null);
  const [candidates, setCandidates] = useState<AIListingCopyCandidate[]>([]);
  const [sourceRequestId, setSourceRequestId] = useState<string>();
  const [selected, setSelected] = useState<number | "rewrite" | null>(null);
  const [instruction, setInstruction] = useState("");
  const [generating, setGenerating] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [pendingAttempt, setPendingAttempt] = useState<PendingAttempt | null>(null);
  const generatingRef = useRef(false);
  const abortRef = useRef<AbortController | null>(null);
  const storageKey = `fractal-ai-listing-copy:${listingId}`;

  useEffect(() => {
    let active = true;
    void platform.ai.allowance().then((value) => {
      if (active) setAllowance(value);
    }).catch(() => {
      // The generation request remains available if the allowance badge could
      // not be loaded; the server is still the authority on quota.
    });
    return () => { active = false; };
  }, []);

  useEffect(() => {
    try {
      const raw = window.sessionStorage.getItem(storageKey);
      if (!raw) return;
      const parsed = JSON.parse(raw) as Partial<PendingAttempt>;
      if (
        typeof parsed.key === "string"
        && parsed.key.length > 0
        && parsed.body?.listingId === listingId
        && parsed.body.locale === locale
      ) {
        setPendingAttempt(parsed as PendingAttempt);
        setError(t("errors.interrupted"));
      } else {
        window.sessionStorage.removeItem(storageKey);
      }
    } catch {
      window.sessionStorage.removeItem(storageKey);
    }
  }, [listingId, locale, storageKey, t]);

  useEffect(() => () => {
    abortRef.current?.abort();
    onBusyChange?.(false);
  }, [onBusyChange]);

  const exhausted = allowance?.member === false && allowance.remaining === 0;
  const disabled = allowance?.enabled === false;

  const rememberAttempt = (attempt: PendingAttempt | null) => {
    setPendingAttempt(attempt);
    try {
      if (attempt) window.sessionStorage.setItem(storageKey, JSON.stringify(attempt));
      else window.sessionStorage.removeItem(storageKey);
    } catch {
      // A privacy-hardened browser may disable sessionStorage. The in-memory
      // key still makes retries safe for the lifetime of this dialog.
    }
  };

  const setBusy = (busy: boolean) => {
    generatingRef.current = busy;
    setGenerating(busy);
    onBusyChange?.(busy);
  };

  const generate = async (revision?: string, retry = false) => {
    if (generatingRef.current) return;
    const attempt = retry && pendingAttempt
      ? pendingAttempt
      : {
          body: {
            listingId,
            locale,
            ...(revision && sourceRequestId
              ? { sourceRequestId, instruction: revision }
              : {}),
          },
          key: crypto.randomUUID(),
        } satisfies PendingAttempt;
    rememberAttempt(attempt);
    const controller = new AbortController();
    abortRef.current = controller;
    setBusy(true);
    setError(null);
    try {
      const result = await platform.ai.listingCopy(
        attempt.body,
        attempt.key,
        controller.signal,
      );
      setCandidates(result.candidates.slice(0, 3));
      setSourceRequestId(result.requestId);
      setAllowance(result.allowance);
      setSelected(null);
      rememberAttempt(null);
      if (attempt.body.instruction) setInstruction("");
    } catch (reason) {
      if (reason instanceof DOMException && reason.name === "AbortError") {
        setError(t("errors.interrupted"));
      } else if (reason instanceof PlatformApiError && reason.status === 402) {
        rememberAttempt(null);
        setError(t("errors.exhausted"));
        setAllowance((current) => current ? { ...current, remaining: 0 } : current);
      } else if (reason instanceof PlatformApiError && reason.code === "AI_DISABLED") {
        rememberAttempt(null);
        setError(t("errors.disabled"));
      } else if (reason instanceof PlatformApiError && reason.code === "AI_PROVIDER_UNAVAILABLE") {
        setError(t("errors.unavailable"));
      } else {
        setError(t("errors.failed"));
      }
    } finally {
      if (abortRef.current === controller) {
        abortRef.current = null;
        setBusy(false);
      }
    }
  };

  const selectCandidate = (candidate: AIListingCopyCandidate, index: number) => {
    setSelected(index);
    onApply(candidate);
  };

  return (
    <section
      className="rounded-sm border border-brand/20 bg-brand/[0.035] p-3.5"
      aria-labelledby="ai-listing-copy-title"
      aria-busy={generating}
    >
      <div className="flex flex-wrap items-start justify-between gap-3">
        <div className="space-y-1">
          <div className="flex items-center gap-2">
            <Sparkles className="h-4 w-4 text-brand" aria-hidden="true" />
            <h3 id="ai-listing-copy-title" className="text-sm font-medium text-ink/90">{t("title")}</h3>
          </div>
          <p className="text-xs leading-5 text-muted-foreground">{t("hint")}</p>
        </div>
        <div className="flex items-center gap-2">
          {allowance && (
            <Badge variant={exhausted ? "warning" : "outline"}>
              {allowance.member
                ? t("allowanceMember")
                : t("allowanceRemaining", { count: allowance.remaining ?? 0 })}
            </Badge>
          )}
          {generating ? (
            <Button
              type="button"
              size="sm"
              variant="outline"
              onClick={() => abortRef.current?.abort()}
            >
              <Square className="mr-1.5 h-3 w-3 fill-current" aria-hidden="true" />
              {t("stop")}
            </Button>
          ) : (
            <Button
              type="button"
              size="sm"
              variant="fractal"
              disabled={disabled || (!pendingAttempt && exhausted)}
              onClick={() => void generate(undefined, Boolean(pendingAttempt))}
            >
              {pendingAttempt
                ? t("retry")
                : candidates.length > 0 ? t("generateAgain") : t("generate")}
            </Button>
          )}
        </div>
      </div>

      {error && <p role="alert" className="mt-3 text-xs text-red-400">{error}</p>}
      {!error && disabled && (
        <p role="status" className="mt-3 text-xs text-muted-foreground">{t("errors.disabled")}</p>
      )}

      {candidates.length > 0 && (
        <div className="mt-4 space-y-2" role="radiogroup" aria-label={t("choicesLabel")}>
          {candidates.map((candidate, index) => (
            <button
              key={`${sourceRequestId ?? "candidate"}-${index}`}
              type="button"
              role="radio"
              aria-checked={selected === index}
              className={cn(
                "w-full rounded-sm border p-3 text-left transition-colors focus-visible:outline-none focus-visible:border-brand focus-visible:ring-1 focus-visible:ring-brand/20",
                selected === index
                  ? "border-brand/55 bg-brand/[0.08]"
                  : "border-instrument-rule bg-instrument/45 hover:border-brand/30",
              )}
              onClick={() => selectCandidate(candidate, index)}
            >
              <div className="flex items-start gap-2.5">
                <span className={cn(
                  "mt-0.5 flex h-4 w-4 shrink-0 items-center justify-center rounded-full border",
                  selected === index ? "border-brand bg-brand text-background" : "border-ink/25",
                )}>
                  {selected === index && <Check className="h-3 w-3" aria-hidden="true" />}
                </span>
                <span className="min-w-0 space-y-1.5">
                  <span className="block text-sm font-medium text-ink/90">{candidate.title}</span>
                  <span className="block whitespace-pre-wrap text-xs leading-5 text-ink/60">{candidate.description}</span>
                  {candidate.tags.length > 0 && (
                    <span className="flex flex-wrap gap-1.5 pt-0.5">
                      {candidate.tags.map((tag) => (
                        <span key={tag} className="rounded-sm border border-instrument-rule px-1.5 py-0.5 font-mono text-[10px] text-ink/45">
                          #{tag}
                        </span>
                      ))}
                    </span>
                  )}
                </span>
              </div>
            </button>
          ))}

          <button
            type="button"
            role="radio"
            aria-checked={selected === "rewrite"}
            className={cn(
              "w-full rounded-sm border p-3 text-left text-xs transition-colors focus-visible:outline-none focus-visible:border-brand focus-visible:ring-1 focus-visible:ring-brand/20",
              selected === "rewrite"
                ? "border-brand/55 bg-brand/[0.08] text-ink/85"
                : "border-instrument-rule bg-instrument/45 text-ink/60 hover:border-brand/30",
            )}
            onClick={() => setSelected("rewrite")}
          >
            <span className="flex items-center gap-2.5">
              <span className={cn(
                "flex h-4 w-4 shrink-0 items-center justify-center rounded-full border",
                selected === "rewrite" ? "border-brand bg-brand text-background" : "border-ink/25",
              )}>
                {selected === "rewrite" && <Check className="h-3 w-3" aria-hidden="true" />}
              </span>
              {t("notSatisfied")}
            </span>
          </button>

          {selected === "rewrite" && (
            <div className="space-y-2 rounded-sm border border-instrument-rule bg-instrument/30 p-3">
              <textarea
                autoFocus
                className="min-h-20 w-full resize-y rounded-sm border border-instrument-rule bg-transparent p-2.5 text-sm text-ink/80 outline-none placeholder:text-ink/30 focus-visible:border-brand/50"
                placeholder={t("instructionPlaceholder")}
                value={instruction}
                maxLength={1000}
                onChange={(event) => setInstruction(event.target.value)}
              />
              <div className="flex items-center justify-between gap-3">
                <span className="font-mono text-[10px] text-ink/35">{instruction.length}/1000</span>
                <Button
                  type="button"
                  size="sm"
                  variant="fractal"
                  loading={generating}
                  disabled={!instruction.trim() || exhausted || disabled || Boolean(pendingAttempt)}
                  onClick={() => void generate(instruction.trim())}
                >
                  {t("rewrite")}
                </Button>
              </div>
            </div>
          )}
        </div>
      )}

      {candidates.length > 0 && (
        <p className="mt-3 text-[11px] leading-4 text-muted-foreground">{t("applyHint")}</p>
      )}
    </section>
  );
}
