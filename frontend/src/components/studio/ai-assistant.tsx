"use client";

import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import {
  BookOpen,
  Bot,
  Check,
  Compass,
  Image as ImageIcon,
  MessageSquarePlus,
  Palette,
  Pencil,
  RotateCcw,
  Send,
  SlidersHorizontal,
  Sparkles,
  Square,
  ThumbsDown,
  ThumbsUp,
  Trash2,
  Undo2,
  X,
} from "lucide-react";
import { useTranslations } from "next-intl";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import {
  platform,
  PlatformApiError,
  type AIAllowance,
  type AIConversation,
  type AIMessage,
  type AIMessageInput,
  type AIStudioSuggestion,
  type FractalSpec,
  type StudioCapabilities,
} from "@/lib/api/platform";
import { cn } from "@/lib/utils/cn";

export interface StudioAIAssistantProps {
  spec: FractalSpec;
  mode: string;
  output: { width: number; height: number; preset?: string };
  capabilities: StudioCapabilities;
  /** A current in-memory preview or its same-origin/object URL. Never persisted by this component. */
  preview?: Blob | string | null;
  className?: string;
  disabled?: boolean;
  onClose?: () => void;
  onApplySuggestion: (
    patch: Partial<FractalSpec>,
    details: { messageId: string; reason: string; previousSpec: FractalSpec },
  ) => void | Promise<void>;
  onUndoSuggestion: (
    previousSpec: FractalSpec,
    details: { messageId: string },
  ) => void | Promise<void>;
}

interface PreparedRequest extends AIMessageInput {
  conversationId: string;
  idempotencyKey: string;
}

interface AppliedSuggestion {
  previousSpec: FractalSpec;
  busy: boolean;
}

const MAX_IMAGE_BYTES = 1024 * 1024;
const MAX_IMAGE_EDGE = 640;
const MAX_MESSAGE_CHARS = 4000;

function cloneSpec(spec: FractalSpec): FractalSpec {
  if (typeof structuredClone === "function") return structuredClone(spec);
  return JSON.parse(JSON.stringify(spec)) as FractalSpec;
}

function canvasBlob(canvas: HTMLCanvasElement, quality: number): Promise<Blob> {
  return new Promise((resolve, reject) => {
    canvas.toBlob(
      (blob) => blob ? resolve(blob) : reject(new Error("Could not encode Studio preview")),
      "image/jpeg",
      quality,
    );
  });
}

async function previewBlob(preview: Blob | string): Promise<Blob> {
  if (preview instanceof Blob) return preview;
  const url = new URL(preview, window.location.href);
  if (!["blob:", "data:"].includes(url.protocol) && url.origin !== window.location.origin) {
    throw new Error("Only a same-origin Studio preview can be analysed");
  }
  const response = await fetch(url, { credentials: "omit", cache: "no-store" });
  if (!response.ok) throw new Error("Could not read Studio preview");
  return response.blob();
}

/** Downscale in browser memory only; no temporary file, object store, or database write. */
async function preparePreview(preview: Blob | string): Promise<Blob> {
  const source = await previewBlob(preview);
  const bitmap = await createImageBitmap(source);
  try {
    let width = bitmap.width;
    let height = bitmap.height;
    const edgeScale = Math.min(1, MAX_IMAGE_EDGE / Math.max(width, height));
    width = Math.max(1, Math.round(width * edgeScale));
    height = Math.max(1, Math.round(height * edgeScale));

    const canvas = document.createElement("canvas");
    const context = canvas.getContext("2d", { alpha: false });
    if (!context) throw new Error("Could not prepare Studio preview");
    let encoded: Blob | null = null;
    for (let attempt = 0; attempt < 5; attempt += 1) {
      canvas.width = width;
      canvas.height = height;
      context.fillStyle = "#000";
      context.fillRect(0, 0, width, height);
      context.drawImage(bitmap, 0, 0, width, height);
      encoded = await canvasBlob(canvas, Math.max(0.45, 0.86 - attempt * 0.1));
      if (encoded.size <= MAX_IMAGE_BYTES) return encoded;
      width = Math.max(1, Math.round(width * 0.8));
      height = Math.max(1, Math.round(height * 0.8));
    }
    if (!encoded || encoded.size > MAX_IMAGE_BYTES) throw new Error("Studio preview is too large");
    return encoded;
  } finally {
    bitmap.close();
  }
}

function displayValue(value: unknown): string {
  if (value === undefined) return "—";
  if (value === null) return "null";
  if (typeof value === "string" || typeof value === "number" || typeof value === "boolean") return String(value);
  const serialized = JSON.stringify(value);
  return serialized.length > 90 ? `${serialized.slice(0, 87)}…` : serialized;
}

function errorMessage(error: unknown, fallback: string): string {
  if (error instanceof PlatformApiError) return error.message;
  if (error instanceof Error && error.message) return error.message;
  return fallback;
}

export function StudioAIAssistant({
  spec,
  mode,
  output,
  capabilities,
  preview,
  className,
  disabled = false,
  onClose,
  onApplySuggestion,
  onUndoSuggestion,
}: StudioAIAssistantProps) {
  const t = useTranslations("aiAssistant");
  const [conversations, setConversations] = useState<AIConversation[]>([]);
  const [selectedId, setSelectedId] = useState<string | null>(null);
  const [messages, setMessages] = useState<AIMessage[]>([]);
  const [allowance, setAllowance] = useState<AIAllowance | null>(null);
  const [draft, setDraft] = useState("");
  const [loading, setLoading] = useState(true);
  const [sending, setSending] = useState(false);
  const [streamContent, setStreamContent] = useState("");
  const [streamSuggestion, setStreamSuggestion] = useState<AIStudioSuggestion | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [lastRequest, setLastRequest] = useState<PreparedRequest | null>(null);
  const [renaming, setRenaming] = useState(false);
  const [renameTitle, setRenameTitle] = useState("");
  const [feedbackConsent, setFeedbackConsent] = useState(false);
  const [feedback, setFeedback] = useState<Record<string, -1 | 1>>({});
  const [applied, setApplied] = useState<Record<string, AppliedSuggestion>>({});
  const abortRef = useRef<AbortController | null>(null);
  const skipMessageLoadRef = useRef<string | null>(null);
  const messageEndRef = useRef<HTMLDivElement | null>(null);
  const selectedIdRef = useRef<string | null>(selectedId);
  selectedIdRef.current = selectedId;
  const selected = conversations.find((conversation) => conversation.id === selectedId) ?? null;
  const exhausted = allowance?.remaining === 0;
  const unavailable = allowance?.enabled === false;
  const blocked = disabled || unavailable;

  const context = useMemo<AIMessageInput["context"]>(() => ({
    spec,
    mode,
    output,
    capabilities,
  }), [capabilities, mode, output, spec]);

  const refreshConversations = useCallback(async () => {
    const next = await platform.ai.conversations();
    setConversations(next);
    return next;
  }, []);

  const refreshMessages = useCallback(async (conversationId: string) => {
    const next = await platform.ai.messages(conversationId);
    if (selectedIdRef.current !== conversationId) return next;
    setMessages(next);
    const knownFeedback: Record<string, -1 | 1> = {};
    for (const message of next) {
      if (message.feedback === -1 || message.feedback === 1) knownFeedback[message.id] = message.feedback;
      else if (message.feedback) knownFeedback[message.id] = message.feedback.rating;
    }
    setFeedback(knownFeedback);
    return next;
  }, []);

  useEffect(() => {
    let active = true;
    void Promise.all([platform.ai.conversations(), platform.ai.allowance()])
      .then(([nextConversations, nextAllowance]) => {
        if (!active) return;
        setConversations(nextConversations);
        setAllowance(nextAllowance);
        setSelectedId((current) => current ?? nextConversations[0]?.id ?? null);
      })
      .catch((reason: unknown) => {
        if (active) setError(errorMessage(reason, t("errors.load")));
      })
      .finally(() => {
        if (active) setLoading(false);
      });
    return () => {
      active = false;
      abortRef.current?.abort();
    };
  }, [t]);

  useEffect(() => {
    if (!selectedId) {
      setMessages([]);
      return;
    }
    if (skipMessageLoadRef.current === selectedId) {
      skipMessageLoadRef.current = null;
      return;
    }
    let active = true;
    setLoading(true);
    void refreshMessages(selectedId)
      .then(() => {
        if (!active) return;
      })
      .catch((reason: unknown) => {
        if (active) setError(errorMessage(reason, t("errors.load")));
      })
      .finally(() => {
        if (active) setLoading(false);
      });
    return () => {
      active = false;
    };
  }, [refreshMessages, selectedId, t]);

  useEffect(() => {
    setFeedbackConsent(selected?.optimizationConsent ?? false);
  }, [selected?.id, selected?.optimizationConsent]);

  useEffect(() => {
    messageEndRef.current?.scrollIntoView({ block: "end", behavior: streamContent ? "auto" : "smooth" });
  }, [messages, streamContent, streamSuggestion]);

  const createConversation = useCallback(async (): Promise<AIConversation> => {
    const created = await platform.ai.createConversation(t("newConversation"));
    skipMessageLoadRef.current = created.id;
    setConversations((current) => [created, ...current]);
    setSelectedId(created.id);
    setMessages([]);
    setError(null);
    return created;
  }, [t]);

  const switchConversation = (conversationId: string) => {
    if (sending || conversationId === selectedId) return;
    setSelectedId(conversationId);
    setMessages([]);
    setFeedback({});
    setStreamContent("");
    setStreamSuggestion(null);
    setError(null);
    setLastRequest(null);
    setRenaming(false);
  };

  const saveRename = async () => {
    if (!selected || !renameTitle.trim()) return;
    try {
      const updated = await platform.ai.updateConversation(selected.id, { title: renameTitle.trim() });
      setConversations((current) => current.map((item) => item.id === updated.id ? updated : item));
      setRenaming(false);
    } catch (reason) {
      setError(errorMessage(reason, t("errors.rename")));
    }
  };

  const removeConversation = async () => {
    if (!selected || !window.confirm(t("deleteConfirm"))) return;
    try {
      await platform.ai.deleteConversation(selected.id);
      const next = conversations.filter((item) => item.id !== selected.id);
      setConversations(next);
      setSelectedId(next[0]?.id ?? null);
      setMessages([]);
      setLastRequest(null);
      setError(null);
    } catch (reason) {
      setError(errorMessage(reason, t("errors.delete")));
    }
  };

  const runRequest = useCallback(async (request: PreparedRequest, retry: boolean) => {
    const controller = new AbortController();
    abortRef.current = controller;
    setSending(true);
    setError(null);
    setStreamContent("");
    setStreamSuggestion(null);
    if (!retry) {
      setMessages((current) => [
        ...current,
        {
          id: `pending-${request.idempotencyKey}`,
          conversationId: request.conversationId,
          role: "user",
          content: request.text,
          suggestion: null,
          createdAt: new Date().toISOString(),
        },
      ]);
    }
    let streamError: string | null = null;
    let streamedText = "";
    let streamedSuggestion: AIStudioSuggestion | null = null;
    try {
      await platform.ai.streamMessage(
        request.conversationId,
        { ...request, signal: controller.signal },
        (event) => {
          if (event.type === "message") {
            setMessages((current) => current.map((message) =>
              message.id === `pending-${request.idempotencyKey}`
                ? event.message ?? { ...message, id: event.messageId }
                : message,
            ));
          } else if (event.type === "delta") {
            streamedText += event.content;
            setStreamContent((current) => current + event.content);
          } else if (event.type === "suggestion") {
            streamedSuggestion = event.suggestion;
            setStreamSuggestion(event.suggestion);
          } else if (event.type === "error") {
            streamError = event.code;
            setError(event.code === "AI_PROVIDER_UNAVAILABLE" ? t("errors.provider") : event.message);
          } else if (event.type === "done") {
            if (event.allowance) setAllowance(event.allowance);
            const completed: AIMessage = {
              id: event.messageId,
              conversationId: request.conversationId,
              role: "assistant",
              content: streamedText || streamedSuggestion?.reason || "",
              suggestion: streamedSuggestion,
              createdAt: new Date().toISOString(),
            };
            setMessages((current) => current.some((message) => message.id === completed.id)
              ? current
              : [...current, completed]);
          }
        },
      );
      if (!streamError) setLastRequest(null);
    } catch (reason) {
      if (!(reason instanceof DOMException && reason.name === "AbortError")) {
        if (reason instanceof PlatformApiError && reason.code === "AI_TRIAL_EXHAUSTED") {
          setError(t("errors.trial"));
        } else if (reason instanceof PlatformApiError && reason.code === "AI_DISABLED") {
          setError(t("errors.disabled"));
        } else if (reason instanceof PlatformApiError && reason.code === "AI_PROVIDER_UNAVAILABLE") {
          setError(t("errors.provider"));
        } else if (reason instanceof PlatformApiError && reason.code === "ai_concurrency_exhausted") {
          setError(t("errors.concurrency"));
        } else if (reason instanceof PlatformApiError && reason.code === "COMPUTE_CAPACITY_EXHAUSTED") {
          setError(t("errors.compute"));
        } else {
          setError(errorMessage(reason, t("errors.send")));
        }
      }
    } finally {
      abortRef.current = null;
      setSending(false);
      setStreamContent("");
      setStreamSuggestion(null);
      void Promise.all([
        refreshMessages(request.conversationId),
        platform.ai.allowance().then(setAllowance),
        refreshConversations(),
      ]).catch((reason: unknown) => setError(errorMessage(reason, t("errors.load"))));
    }
  }, [refreshConversations, refreshMessages, t]);

  const submit = useCallback(async (text: string, includePreview = false, requestPatch = false) => {
    const normalized = text.trim();
    if (!normalized || sending || blocked || exhausted) return;
    try {
      const conversation = selected ?? await createConversation();
      const image = includePreview && preview ? await preparePreview(preview) : null;
      const request: PreparedRequest = {
        conversationId: conversation.id,
        text: normalized,
        context,
        image,
        requestPatch,
        idempotencyKey: crypto.randomUUID(),
      };
      setLastRequest(request);
      setDraft("");
      await runRequest(request, false);
    } catch (reason) {
      if (reason instanceof PlatformApiError && reason.code === "AI_TRIAL_EXHAUSTED") setError(t("errors.trial"));
      else if (reason instanceof PlatformApiError && reason.code === "AI_DISABLED") setError(t("errors.disabled"));
      else if (reason instanceof PlatformApiError && reason.code === "AI_PROVIDER_UNAVAILABLE") setError(t("errors.provider"));
      else if (reason instanceof PlatformApiError && reason.code === "ai_concurrency_exhausted") setError(t("errors.concurrency"));
      else setError(errorMessage(reason, t("errors.send")));
    }
  }, [blocked, context, createConversation, exhausted, preview, runRequest, selected, sending, t]);

  const retry = () => {
    if (!lastRequest || sending) return;
    void runRequest(lastRequest, true);
  };

  const sendFeedback = async (messageId: string, rating: -1 | 1) => {
    try {
      await platform.ai.feedback(messageId, rating, feedbackConsent);
      setFeedback((current) => ({ ...current, [messageId]: rating }));
      if (feedbackConsent && selected) {
        setConversations((current) => current.map((item) =>
          item.id === selected.id ? { ...item, optimizationConsent: true } : item,
        ));
      }
    } catch (reason) {
      setError(errorMessage(reason, t("errors.feedback")));
    }
  };

  const setConsent = async (consent: boolean) => {
    if (!selected) return;
    setFeedbackConsent(consent);
    // Opting in is recorded only together with an explicit thumbs-up/down.
    // An already-recorded conversation can still be opted out immediately.
    if (consent || !selected.optimizationConsent) return;
    try {
      const updated = await platform.ai.updateConversation(selected.id, { optimizationConsent: false });
      setConversations((current) => current.map((item) => item.id === updated.id ? updated : item));
    } catch (reason) {
      setFeedbackConsent(true);
      setError(errorMessage(reason, t("errors.consent")));
    }
  };

  const applySuggestion = async (messageId: string, suggestion: AIStudioSuggestion) => {
    const previousSpec = cloneSpec(spec);
    const previousApplied = applied;
    setApplied({ [messageId]: { previousSpec, busy: true } });
    try {
      await onApplySuggestion(suggestion.patch, { messageId, reason: suggestion.reason, previousSpec });
      setApplied({ [messageId]: { previousSpec, busy: false } });
    } catch (reason) {
      setApplied(previousApplied);
      setError(errorMessage(reason, t("errors.apply")));
    }
  };

  const undoSuggestion = async (messageId: string) => {
    const record = applied[messageId];
    if (!record || record.busy) return;
    setApplied((current) => ({ ...current, [messageId]: { ...record, busy: true } }));
    try {
      await onUndoSuggestion(record.previousSpec, { messageId });
      setApplied((current) => {
        const next = { ...current };
        delete next[messageId];
        return next;
      });
    } catch (reason) {
      setApplied((current) => ({ ...current, [messageId]: { ...record, busy: false } }));
      setError(errorMessage(reason, t("errors.undo")));
    }
  };

  const quickActions = [
    { key: "palette", icon: Palette, image: false, requestPatch: true },
    { key: "explore", icon: Compass, image: false, requestPatch: true },
    { key: "parameters", icon: SlidersHorizontal, image: false, requestPatch: false },
    { key: "knowledge", icon: BookOpen, image: false, requestPatch: false },
    { key: "analyse", icon: ImageIcon, image: true, requestPatch: true },
  ] as const;

  return (
    <section className={cn("flex min-h-0 flex-col border border-instrument-rule bg-instrument", className)} aria-label={t("title")}>
      <header className="flex items-center gap-2 border-b border-instrument-rule px-3 py-2.5">
        <span className="flex h-7 w-7 items-center justify-center border border-brand/35 bg-brand/10 text-brand">
          <Bot className="h-4 w-4" />
        </span>
        <div className="min-w-0 flex-1">
          <h2 className="font-mono text-[11px] font-medium uppercase tracking-[0.12em] text-ink/80">{t("title")}</h2>
          <p className="text-[11px] text-ink/45">
            {unavailable
              ? t("allowance.unavailable")
              : allowance?.member
              ? t("allowance.member")
              : allowance ? t("allowance.remaining", { count: allowance.remaining ?? 0 }) : t("allowance.loading")}
          </p>
        </div>
        {onClose && (
          <Button type="button" size="icon" variant="ghost" aria-label={t("close")} onClick={onClose}>
            <X className="h-4 w-4" />
          </Button>
        )}
      </header>

      <div className="flex items-center gap-1.5 border-b border-instrument-rule p-2">
        {renaming && selected ? (
          <>
            <Input
              className="min-w-0 flex-1"
              value={renameTitle}
              maxLength={120}
              autoFocus
              onChange={(event) => setRenameTitle(event.target.value)}
              onKeyDown={(event) => {
                if (event.key === "Enter") void saveRename();
                if (event.key === "Escape") setRenaming(false);
              }}
            />
            <Button type="button" size="icon" variant="ghost" aria-label={t("renameSave")} onClick={() => void saveRename()}>
              <Check className="h-3.5 w-3.5" />
            </Button>
            <Button type="button" size="icon" variant="ghost" aria-label={t("renameCancel")} onClick={() => setRenaming(false)}>
              <X className="h-3.5 w-3.5" />
            </Button>
          </>
        ) : (
          <>
            <select
              className="instrument-control h-8 min-w-0 flex-1 px-2 text-xs"
              value={selectedId ?? ""}
              disabled={sending || conversations.length === 0}
              aria-label={t("conversation")}
              onChange={(event) => switchConversation(event.target.value)}
            >
              {conversations.length === 0 && <option value="">{t("newConversation")}</option>}
              {conversations.map((conversation) => (
                <option key={conversation.id} value={conversation.id}>{conversation.title}</option>
              ))}
            </select>
            <Button
              type="button"
              size="icon"
              variant="ghost"
              disabled={sending}
              aria-label={t("newConversation")}
              onClick={() => void createConversation().catch((reason: unknown) => setError(errorMessage(reason, t("errors.create"))))}
            >
              <MessageSquarePlus className="h-3.5 w-3.5" />
            </Button>
            <Button
              type="button"
              size="icon"
              variant="ghost"
              disabled={sending || !selected}
              aria-label={t("rename")}
              onClick={() => {
                if (!selected) return;
                setRenameTitle(selected.title);
                setRenaming(true);
              }}
            >
              <Pencil className="h-3.5 w-3.5" />
            </Button>
            <Button
              type="button"
              size="icon"
              variant="ghost"
              disabled={sending || !selected}
              aria-label={t("delete")}
              onClick={() => void removeConversation()}
            >
              <Trash2 className="h-3.5 w-3.5" />
            </Button>
          </>
        )}
      </div>

      <div className="min-h-0 flex-1 space-y-3 overflow-y-auto px-3 py-3" aria-live="polite">
        {loading && messages.length === 0 && <p className="text-center text-xs text-ink/45">{t("loading")}</p>}
        {!loading && messages.length === 0 && !streamContent && (
          <div className="flex min-h-full flex-col items-center justify-center gap-4 py-6 text-center">
            <div>
              <Sparkles className="mx-auto mb-2 h-5 w-5 text-brand/70" />
              <p className="text-sm text-ink/75">{t("empty.title")}</p>
              <p className="mt-1 max-w-[28rem] text-xs leading-5 text-ink/45">{t("empty.description")}</p>
            </div>
            <div className="grid w-full grid-cols-2 gap-1.5">
              {quickActions.map(({ key, icon: Icon, image, requestPatch }) => (
                <Button
                  key={key}
                  type="button"
                  className={cn("h-auto min-h-9 justify-start whitespace-normal px-2 py-2 text-left normal-case tracking-normal", key === "analyse" && "col-span-2")}
                  size="sm"
                  variant="outline"
                  disabled={blocked || exhausted || sending || (image && !preview)}
                  onClick={() => void submit(t(`quick.${key}.prompt`), image, requestPatch)}
                >
                  <Icon className="h-3.5 w-3.5 shrink-0" />
                  {t(`quick.${key}.label`)}
                </Button>
              ))}
            </div>
          </div>
        )}

        {messages.map((message) => (
          <article key={message.id} className={cn("flex", message.role === "user" ? "justify-end" : "justify-start")}>
            <div className={cn(
              "max-w-[92%] border px-3 py-2 text-sm leading-6",
              message.role === "user"
                ? "border-brand/25 bg-brand/[0.07] text-ink/80"
                : "border-instrument-rule bg-instrument-raised text-ink/75",
            )}>
              <p className="whitespace-pre-wrap break-words">{message.content}</p>
              {message.role === "assistant" && message.suggestion && (
                <SuggestionCard
                  messageId={message.id}
                  suggestion={message.suggestion}
                  currentSpec={applied[message.id]?.previousSpec ?? spec}
                  applied={applied[message.id]}
                  onApply={applySuggestion}
                  onUndo={undoSuggestion}
                />
              )}
              {message.role === "assistant" && !message.id.startsWith("pending-") && (
                <div className="mt-2 flex items-center gap-1 border-t border-instrument-rule pt-1.5">
                  <Button
                    type="button"
                    size="icon"
                    variant="ghost"
                    className={cn("h-6 w-6", feedback[message.id] === 1 && "text-brand")}
                    aria-label={t("feedback.good")}
                    onClick={() => void sendFeedback(message.id, 1)}
                  >
                    <ThumbsUp className="h-3 w-3" />
                  </Button>
                  <Button
                    type="button"
                    size="icon"
                    variant="ghost"
                    className={cn("h-6 w-6", feedback[message.id] === -1 && "text-red-400")}
                    aria-label={t("feedback.bad")}
                    onClick={() => void sendFeedback(message.id, -1)}
                  >
                    <ThumbsDown className="h-3 w-3" />
                  </Button>
                </div>
              )}
            </div>
          </article>
        ))}

        {(sending || streamContent || streamSuggestion) && (
          <article className="flex justify-start">
            <div className="max-w-[92%] border border-instrument-rule bg-instrument-raised px-3 py-2 text-sm leading-6 text-ink/75">
              {streamContent
                ? <p className="whitespace-pre-wrap break-words">{streamContent}<span className="ml-0.5 animate-pulse text-brand">▍</span></p>
                : <p className="flex items-center gap-2 text-xs text-ink/45"><Sparkles className="h-3.5 w-3.5 animate-pulse" />{t("thinking")}</p>}
              {streamSuggestion && (
                <SuggestionCard
                  messageId="streaming"
                  suggestion={streamSuggestion}
                  currentSpec={spec}
                  applied={undefined}
                  disabled
                  onApply={applySuggestion}
                  onUndo={undoSuggestion}
                />
              )}
            </div>
          </article>
        )}
        <div ref={messageEndRef} />
      </div>

      {error && (
        <div className="flex items-start gap-2 border-t border-red-500/25 bg-red-500/[0.04] px-3 py-2 text-xs text-red-400">
          <span className="min-w-0 flex-1">{error}</span>
          {lastRequest && !sending && (
            <Button type="button" size="sm" variant="ghost" className="h-6 text-red-400" onClick={retry}>
              <RotateCcw className="h-3 w-3" />{t("retry")}
            </Button>
          )}
          <button type="button" aria-label={t("dismissError")} onClick={() => setError(null)}><X className="h-3.5 w-3.5" /></button>
        </div>
      )}

      <footer className="space-y-2 border-t border-instrument-rule p-2.5">
        {selected && (
          <label className="flex cursor-pointer items-start gap-2 text-[10px] leading-4 text-ink/45">
            <input
              type="checkbox"
              className="mt-0.5 accent-amber-500"
              checked={feedbackConsent}
              disabled={sending}
              onChange={(event) => void setConsent(event.target.checked)}
            />
            <span>{t("feedback.consent")}</span>
          </label>
        )}
        <div className="flex items-end gap-1.5">
          <div className="relative min-w-0 flex-1">
            <textarea
              className="block max-h-32 min-h-[66px] w-full resize-y rounded-sm border border-instrument-rule bg-instrument-raised px-2.5 py-2 pr-10 text-sm text-ink/80 placeholder:text-ink/30 focus:border-brand/60 focus:outline-none"
              value={draft}
              maxLength={MAX_MESSAGE_CHARS}
              disabled={blocked || sending || exhausted}
              placeholder={unavailable ? t("unavailable") : exhausted ? t("trialExhausted") : t("placeholder")}
              onChange={(event) => setDraft(event.target.value)}
              onKeyDown={(event) => {
                if (event.key === "Enter" && !event.shiftKey) {
                  event.preventDefault();
                  void submit(draft);
                }
              }}
            />
            {draft.length > MAX_MESSAGE_CHARS * 0.8 && (
              <span className="absolute bottom-1.5 right-2 font-mono text-[9px] text-ink/35">{draft.length}/{MAX_MESSAGE_CHARS}</span>
            )}
          </div>
          {sending ? (
            <Button type="button" size="icon" variant="destructive" aria-label={t("stop")} onClick={() => abortRef.current?.abort()}>
              <Square className="h-3.5 w-3.5 fill-current" />
            </Button>
          ) : (
            <Button
              type="button"
              size="icon"
              variant="fractal"
              disabled={blocked || exhausted || !draft.trim()}
              aria-label={t("send")}
              onClick={() => void submit(draft)}
            >
              <Send className="h-3.5 w-3.5" />
            </Button>
          )}
        </div>
        <p className="text-[9px] leading-4 text-ink/35">{t("disclaimer")}</p>
      </footer>
    </section>
  );
}

interface SuggestionCardProps {
  messageId: string;
  suggestion: AIStudioSuggestion;
  currentSpec: FractalSpec;
  applied: AppliedSuggestion | undefined;
  disabled?: boolean;
  onApply: (messageId: string, suggestion: AIStudioSuggestion) => void | Promise<void>;
  onUndo: (messageId: string) => void | Promise<void>;
}

function SuggestionCard({
  messageId,
  suggestion,
  currentSpec,
  applied,
  disabled = false,
  onApply,
  onUndo,
}: SuggestionCardProps) {
  const t = useTranslations("aiAssistant");
  const changes = Object.entries(suggestion.patch);
  return (
    <div className="mt-3 border border-brand/25 bg-brand/[0.04] p-2.5">
      <div className="mb-2 flex items-start gap-2">
        <Sparkles className="mt-0.5 h-3.5 w-3.5 shrink-0 text-brand" />
        <div className="min-w-0">
          <p className="font-mono text-[10px] uppercase tracking-[0.1em] text-brand">{t("suggestion.title")}</p>
          {suggestion.reason && <p className="mt-0.5 text-xs leading-5 text-ink/55">{suggestion.reason}</p>}
        </div>
      </div>
      <dl className="space-y-1 border-y border-instrument-rule py-2 font-mono text-[10px]">
        {changes.map(([key, value]) => (
          <div key={key} className="grid grid-cols-[minmax(5rem,0.7fr)_1fr] gap-2">
            <dt className="truncate text-ink/45" title={key}>{key}</dt>
            <dd className="min-w-0 break-all text-ink/65">
              <span className="text-red-400/60 line-through">{displayValue(currentSpec[key as keyof FractalSpec])}</span>
              <span className="mx-1 text-ink/30">→</span>
              <span className="text-brand">{displayValue(value)}</span>
            </dd>
          </div>
        ))}
      </dl>
      <div className="mt-2 flex justify-end">
        {applied ? (
          <Button type="button" size="sm" variant="outline" loading={applied.busy} onClick={() => void onUndo(messageId)}>
            <Undo2 className="h-3 w-3" />{t("suggestion.undo")}
          </Button>
        ) : (
          <Button type="button" size="sm" variant="fractal" disabled={disabled || changes.length === 0} onClick={() => void onApply(messageId, suggestion)}>
            <Check className="h-3 w-3" />{t("suggestion.apply")}
          </Button>
        )}
      </div>
    </div>
  );
}
