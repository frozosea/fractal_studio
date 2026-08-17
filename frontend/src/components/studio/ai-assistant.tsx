"use client";

import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import {
  BookOpen,
  Bot,
  Check,
  Compass,
  Image as ImageIcon,
  Layers3,
  ListX,
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
import { MarkdownMessage } from "@/components/studio/markdown-message";
import {
  platform,
  PlatformApiError,
  type AIAllowance,
  type AIAssistantMode,
  type AIConversation,
  type AIMessage,
  type AIMessageInput,
  type AIStudioCandidateSet,
  type AIStudioPatchSuggestion,
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
const CANDIDATE_PREVIEW_EDGE = 256;
const CANDIDATE_PREVIEW_INTERVAL_MS = 2100;

function isCandidateSet(suggestion: AIStudioSuggestion): suggestion is AIStudioCandidateSet {
  return "kind" in suggestion && suggestion.kind === "candidate_set";
}

function sortedWithoutNull(value: unknown): unknown {
  if (Array.isArray(value)) return value.map(sortedWithoutNull);
  if (value && typeof value === "object") {
    return Object.fromEntries(
      Object.entries(value as Record<string, unknown>)
        .filter(([, child]) => child !== null && child !== undefined)
        .sort(([left], [right]) => left < right ? -1 : left > right ? 1 : 0)
        .map(([key, child]) => [key, sortedWithoutNull(child)]),
    );
  }
  return value;
}

function studioContextSignature(
  spec: FractalSpec,
  mode: string,
  output: { width: number; height: number },
): string {
  return JSON.stringify({
    spec: sortedWithoutNull(spec),
    mode,
    output: { width: output.width, height: output.height },
  });
}

function suggestionContextSignature(suggestion: AIStudioSuggestion): string | null {
  if (!suggestion.baseSpec || !suggestion.baseMode || !suggestion.baseOutput) return null;
  return studioContextSignature(suggestion.baseSpec, suggestion.baseMode, suggestion.baseOutput);
}

function throwIfAborted(signal: AbortSignal): void {
  if (signal.aborted) throw new DOMException("AI request stopped", "AbortError");
}

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

async function previewBlob(preview: Blob | string, signal: AbortSignal): Promise<Blob> {
  throwIfAborted(signal);
  if (preview instanceof Blob) return preview;
  const url = new URL(preview, window.location.href);
  if (!["blob:", "data:"].includes(url.protocol) && url.origin !== window.location.origin) {
    throw new Error("Only a same-origin Studio preview can be analysed");
  }
  const response = await fetch(url, { credentials: "omit", cache: "no-store", signal });
  if (!response.ok) throw new Error("Could not read Studio preview");
  return response.blob();
}

/** Downscale in browser memory only; no temporary file, object store, or database write. */
async function preparePreview(preview: Blob | string, signal: AbortSignal): Promise<Blob> {
  const source = await previewBlob(preview, signal);
  throwIfAborted(signal);
  const bitmap = await createImageBitmap(source);
  try {
    throwIfAborted(signal);
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
      throwIfAborted(signal);
      canvas.width = width;
      canvas.height = height;
      context.fillStyle = "#000";
      context.fillRect(0, 0, width, height);
      context.drawImage(bitmap, 0, 0, width, height);
      encoded = await canvasBlob(canvas, Math.max(0.45, 0.86 - attempt * 0.1));
      throwIfAborted(signal);
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
  const inFlightRef = useRef(false);
  const skipMessageLoadRef = useRef<string | null>(null);
  const messageEndRef = useRef<HTMLDivElement | null>(null);
  const selectedIdRef = useRef<string | null>(selectedId);
  selectedIdRef.current = selectedId;
  const selected = conversations.find((conversation) => conversation.id === selectedId) ?? null;
  const exhausted = allowance?.remaining === 0;
  const unavailable = allowance?.enabled === false;
  const blocked = disabled || unavailable;
  const candidateAspectSupported = candidatePreviewDimensions(output) !== null;
  const currentContextSignature = useMemo(
    () => studioContextSignature(spec, mode, output),
    [mode, output, spec],
  );

  const context = useMemo<AIMessageInput["context"]>(() => ({
    spec,
    mode,
    output,
    capabilities,
  }), [capabilities, mode, output, spec]);

  const beginRequest = useCallback((): AbortController | null => {
    if (inFlightRef.current) return null;
    const controller = new AbortController();
    inFlightRef.current = true;
    abortRef.current = controller;
    setSending(true);
    return controller;
  }, []);

  const finishRequest = useCallback((controller: AbortController) => {
    if (abortRef.current !== controller) return;
    abortRef.current = null;
    inFlightRef.current = false;
    setSending(false);
  }, []);

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
      inFlightRef.current = false;
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
    if (inFlightRef.current || conversationId === selectedId) return;
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

  const removeAllConversations = async () => {
    if (conversations.length === 0 || !window.confirm(t("deleteAllConfirm"))) return;
    try {
      await platform.ai.deleteAllConversations();
      setConversations([]);
      setSelectedId(null);
      setMessages([]);
      setFeedback({});
      setLastRequest(null);
      setError(null);
      setRenaming(false);
    } catch (reason) {
      setError(errorMessage(reason, t("errors.delete")));
    }
  };

  const runRequest = useCallback(async (
    request: PreparedRequest,
    retry: boolean,
    controller: AbortController,
  ) => {
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
    let completedResponse = false;
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
            completedResponse = event.partial !== true;
            const completed: AIMessage = {
              id: event.messageId,
              conversationId: request.conversationId,
              role: "assistant",
              content: (event.message?.content ?? streamedText) || (
                streamedSuggestion && !isCandidateSet(streamedSuggestion)
                  ? streamedSuggestion.reason
                  : ""
              ),
              suggestion: event.message?.suggestion ?? streamedSuggestion,
              status: event.partial === true ? "partial" : "completed",
              createdAt: event.message?.createdAt ?? new Date().toISOString(),
            };
            setMessages((current) => current.some((message) => message.id === completed.id)
              ? current
              : [...current, completed]);
          }
        },
      );
      if (completedResponse) {
        setLastRequest((current) => current?.idempotencyKey === request.idempotencyKey ? null : current);
      } else if (!streamError && !controller.signal.aborted) {
        setError(t("errors.incomplete"));
      }
    } catch (reason) {
      if (reason instanceof Error && reason.name === "AbortError") {
        setError(t("errors.stopped"));
      } else {
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
      finishRequest(controller);
      setStreamContent("");
      setStreamSuggestion(null);
      void Promise.all([
        refreshMessages(request.conversationId),
        platform.ai.allowance().then(setAllowance),
        refreshConversations(),
      ]).catch((reason: unknown) => setError(errorMessage(reason, t("errors.load"))));
    }
  }, [finishRequest, refreshConversations, refreshMessages, t]);

  const submit = useCallback(async (
    text: string,
    includePreview = false,
    requestPatch = false,
    assistantMode: AIAssistantMode = "chat",
  ) => {
    const normalized = text.trim();
    if (!normalized || inFlightRef.current || blocked || exhausted) return;
    const controller = beginRequest();
    if (!controller) return;
    setLastRequest(null);
    try {
      const conversation = selected ?? await createConversation();
      throwIfAborted(controller.signal);
      const image = includePreview && preview
        ? await preparePreview(preview, controller.signal)
        : null;
      throwIfAborted(controller.signal);
      const request: PreparedRequest = {
        conversationId: conversation.id,
        text: normalized,
        context,
        image,
        requestPatch,
        assistantMode,
        idempotencyKey: crypto.randomUUID(),
      };
      setLastRequest(request);
      setDraft("");
      await runRequest(request, false, controller);
      // 自动命名:第一条消息成功后,把默认标题换成消息摘要,便于会话列表区分。
      if (conversation.title === t("newConversation")) {
        const title = normalized.length > 20 ? `${normalized.slice(0, 20)}…` : normalized;
        void platform.ai.updateConversation(conversation.id, { title })
          .then(() => refreshConversations())
          .catch(() => {});
      }
    } catch (reason) {
      if (reason instanceof Error && reason.name === "AbortError") setError(t("errors.stopped"));
      else if (reason instanceof PlatformApiError && reason.code === "AI_TRIAL_EXHAUSTED") setError(t("errors.trial"));
      else if (reason instanceof PlatformApiError && reason.code === "AI_DISABLED") setError(t("errors.disabled"));
      else if (reason instanceof PlatformApiError && reason.code === "AI_PROVIDER_UNAVAILABLE") setError(t("errors.provider"));
      else if (reason instanceof PlatformApiError && reason.code === "ai_concurrency_exhausted") setError(t("errors.concurrency"));
      else setError(errorMessage(reason, t("errors.send")));
    } finally {
      // runRequest owns normal completion; this covers conversation/image preparation failures.
      finishRequest(controller);
    }
  }, [beginRequest, blocked, context, createConversation, exhausted, finishRequest, preview, refreshConversations, runRequest, selected, t]);

  const retry = () => {
    if (!lastRequest || inFlightRef.current) return;
    const controller = beginRequest();
    if (!controller) return;
    void runRequest(lastRequest, true, controller);
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

  const applySuggestion = async (messageId: string, suggestion: AIStudioPatchSuggestion) => {
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
    { key: "location", mode: "location", icon: Compass },
    { key: "color", mode: "color", icon: Palette },
    { key: "composition", mode: "composition", icon: Layers3 },
  ] as const;
  const chatActions = [
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
                if (event.key === "Escape") {
                  event.stopPropagation();
                  setRenaming(false);
                }
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
              title={t("newConversation")}
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
              title={t("rename")}
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
              title={t("delete")}
              onClick={() => void removeConversation()}
            >
              <Trash2 className="h-3.5 w-3.5" />
            </Button>
            <Button
              type="button"
              size="icon"
              variant="ghost"
              disabled={sending || conversations.length === 0}
              aria-label={t("deleteAll")}
              title={t("deleteAll")}
              onClick={() => void removeAllConversations()}
            >
              <ListX className="h-3.5 w-3.5" />
            </Button>
          </>
        )}
      </div>

      <div className="grid grid-cols-3 gap-1.5 border-b border-instrument-rule p-2">
        {quickActions.map(({ key, mode: assistantMode, icon: Icon }) => (
          <Button
            key={key}
            type="button"
            className="h-auto min-h-12 flex-col whitespace-normal px-1.5 py-2 text-center normal-case tracking-normal"
            size="sm"
            variant="outline"
            disabled={blocked || exhausted || sending || !preview || !candidateAspectSupported}
            onClick={() => void submit(t(`quick.${key}.prompt`), true, true, assistantMode)}
          >
            <Icon className="h-3.5 w-3.5 shrink-0" />
            {t(`quick.${key}.label`)}
          </Button>
        ))}
      </div>

      {!candidateAspectSupported && (
        <p role="alert" className="border-b border-amber-500/25 bg-amber-500/[0.04] px-3 py-2 text-[10px] leading-4 text-amber-300">
          {t("candidates.aspectUnsupported")}
        </p>
      )}

      <div className="flex flex-wrap gap-1 border-b border-instrument-rule px-2 py-1.5">
        {chatActions.map(({ key, icon: Icon, image, requestPatch }) => (
          <Button
            key={key}
            type="button"
            className="h-7 flex-1 whitespace-nowrap px-1.5 text-[10px] normal-case tracking-normal"
            size="sm"
            variant="ghost"
            disabled={blocked || exhausted || sending || (image && !preview)}
            onClick={() => void submit(t(`quick.${key}.prompt`), image, requestPatch)}
          >
            <Icon className="h-3 w-3 shrink-0" />
            {t(`quick.${key}.label`)}
          </Button>
        ))}
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
              {message.role === "assistant" ? (
                <MarkdownMessage content={message.content} />
              ) : (
                <p className="whitespace-pre-wrap break-words">{message.content}</p>
              )}
              {message.role === "assistant" && message.suggestion && (
                isCandidateSet(message.suggestion) ? (
                  <CandidateSetCard
                    messageId={message.id}
                    suggestion={message.suggestion}
                    currentSpec={spec}
                    currentContextSignature={currentContextSignature}
                    output={output}
                    baselinePreview={typeof preview === "string" ? preview : undefined}
                    applied={applied}
                    onApply={applySuggestion}
                    onUndo={undoSuggestion}
                  />
                ) : (
                  <SuggestionCard
                    messageId={message.id}
                    suggestion={message.suggestion}
                    currentSpec={applied[message.id]?.previousSpec ?? spec}
                    currentContextSignature={currentContextSignature}
                    applied={applied[message.id]}
                    onApply={applySuggestion}
                    onUndo={undoSuggestion}
                  />
                )
              )}
              {message.role === "assistant" && message.status === "completed" && !message.id.startsWith("pending-") && (
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
                isCandidateSet(streamSuggestion) ? (
                  <CandidateSetCard
                    messageId="streaming"
                    suggestion={streamSuggestion}
                    currentSpec={spec}
                    currentContextSignature={currentContextSignature}
                    output={output}
                    baselinePreview={typeof preview === "string" ? preview : undefined}
                    applied={applied}
                    disabled
                    onApply={applySuggestion}
                    onUndo={undoSuggestion}
                  />
                ) : (
                  <SuggestionCard
                    messageId="streaming"
                    suggestion={streamSuggestion}
                    currentSpec={spec}
                    currentContextSignature={currentContextSignature}
                    applied={undefined}
                    disabled
                    onApply={applySuggestion}
                    onUndo={undoSuggestion}
                  />
                )
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
  suggestion: AIStudioPatchSuggestion;
  currentSpec: FractalSpec;
  currentContextSignature: string;
  applied: AppliedSuggestion | undefined;
  disabled?: boolean;
  onApply: (messageId: string, suggestion: AIStudioPatchSuggestion) => void | Promise<void>;
  onUndo: (messageId: string) => void | Promise<void>;
}

type CandidatePreviewState =
  | { status: "loading" }
  | { status: "ready"; url: string }
  | { status: "error" };

function candidatePreviewDimensions(
  output: { width: number; height: number },
): { width: number; height: number } | null {
  const factor = Math.min(1, CANDIDATE_PREVIEW_EDGE / Math.max(output.width, output.height));
  const dimensions = {
    width: Math.round(output.width * factor),
    height: Math.round(output.height * factor),
  };
  // The Platform preview contract requires both edges to be at least 64px.
  // Stretching an extreme aspect ratio to meet that bound would invalidate a
  // composition comparison, so fail visibly instead.
  return dimensions.width >= 64 && dimensions.height >= 64 ? dimensions : null;
}

function abortableDelay(milliseconds: number, signal: AbortSignal): Promise<void> {
  if (milliseconds <= 0) return Promise.resolve();
  return new Promise((resolve, reject) => {
    const timer = window.setTimeout(() => {
      signal.removeEventListener("abort", stop);
      resolve();
    }, milliseconds);
    const stop = () => {
      window.clearTimeout(timer);
      reject(new DOMException("Candidate previews stopped", "AbortError"));
    };
    signal.addEventListener("abort", stop, { once: true });
  });
}

interface CandidateSetCardProps {
  messageId: string;
  suggestion: AIStudioCandidateSet;
  currentSpec: FractalSpec;
  currentContextSignature: string;
  output: { width: number; height: number };
  baselinePreview?: string;
  applied: Record<string, AppliedSuggestion>;
  disabled?: boolean;
  onApply: (messageId: string, suggestion: AIStudioPatchSuggestion) => void | Promise<void>;
  onUndo: (messageId: string) => void | Promise<void>;
}

function CandidateSetCard({
  messageId,
  suggestion,
  currentSpec,
  currentContextSignature,
  output,
  baselinePreview,
  applied,
  disabled = false,
  onApply,
  onUndo,
}: CandidateSetCardProps) {
  const t = useTranslations("aiAssistant");
  const [previews, setPreviews] = useState<Record<string, CandidatePreviewState>>({});
  const [previewing, setPreviewing] = useState(false);
  const [previewError, setPreviewError] = useState<string | null>(null);
  const previewAbortRef = useRef<AbortController | null>(null);
  const previewUrlsRef = useRef(new Set<string>());
  const currentSpecRef = useRef(currentSpec);
  const currentContextSignatureRef = useRef(currentContextSignature);
  currentSpecRef.current = currentSpec;
  currentContextSignatureRef.current = currentContextSignature;

  const clearPreviews = useCallback(() => {
    for (const url of previewUrlsRef.current) URL.revokeObjectURL(url);
    previewUrlsRef.current.clear();
    setPreviews({});
  }, []);

  useEffect(() => () => {
    previewAbortRef.current?.abort();
    for (const url of previewUrlsRef.current) URL.revokeObjectURL(url);
    previewUrlsRef.current.clear();
  }, []);

  const baseContextSignature = useMemo(
    () => suggestionContextSignature(suggestion),
    [suggestion],
  );
  const stale = baseContextSignature === null || currentContextSignature !== baseContextSignature;
  const hasAppliedCandidate = suggestion.candidates.some(
    (candidate) => Boolean(applied[`${messageId}:${candidate.id}`]),
  );
  const previewDimensions = useMemo(() => candidatePreviewDimensions(output), [output]);

  useEffect(() => {
    previewAbortRef.current?.abort();
    clearPreviews();
    setPreviewError(null);
    setPreviewing(false);
  }, [baseContextSignature, clearPreviews]);

  useEffect(() => {
    // Applying one of this card's already-previewed candidates intentionally
    // changes the Studio baseline. Keep that contact sheet visible as the
    // user's visual evidence while disabling sibling applies; unrelated stale
    // history can release its object URLs immediately.
    if (!stale || hasAppliedCandidate) return;
    previewAbortRef.current?.abort();
    clearPreviews();
    setPreviewing(false);
  }, [clearPreviews, hasAppliedCandidate, stale]);

  const generatePreviews = async () => {
    if (disabled || stale || previewing || !previewDimensions || !baseContextSignature) return;
    const controller = new AbortController();
    previewAbortRef.current?.abort();
    previewAbortRef.current = controller;
    clearPreviews();
    setPreviewError(null);
    setPreviewing(true);
    const dimensions = previewDimensions;
    let lastRequestStartedAt = 0;
    try {
      for (const candidate of suggestion.candidates) {
        if (currentContextSignatureRef.current !== baseContextSignature) {
          controller.abort();
          break;
        }
        await abortableDelay(
          Math.max(0, CANDIDATE_PREVIEW_INTERVAL_MS - (Date.now() - lastRequestStartedAt)),
          controller.signal,
        );
        if (controller.signal.aborted) break;
        setPreviews((current) => ({ ...current, [candidate.id]: { status: "loading" } }));
        lastRequestStartedAt = Date.now();
        try {
          const blob = await platform.studio.preview(
            { ...currentSpecRef.current, ...candidate.patch },
            dimensions.width,
            dimensions.height,
            controller.signal,
            "ai_candidate",
          );
          if (controller.signal.aborted) break;
          const url = URL.createObjectURL(blob);
          previewUrlsRef.current.add(url);
          setPreviews((current) => ({ ...current, [candidate.id]: { status: "ready", url } }));
        } catch (reason) {
          if (reason instanceof Error && reason.name === "AbortError") throw reason;
          setPreviews((current) => ({ ...current, [candidate.id]: { status: "error" } }));
          setPreviewError(t("errors.candidatePreview"));
        }
      }
    } catch (reason) {
      if (!(reason instanceof Error && reason.name === "AbortError")) {
        setPreviewError(errorMessage(reason, t("errors.candidatePreview")));
      }
    } finally {
      if (previewAbortRef.current === controller) {
        previewAbortRef.current = null;
        setPreviewing(false);
      }
    }
  };

  const everyPreviewReady = suggestion.candidates.every(
    (candidate) => previews[candidate.id]?.status === "ready",
  );

  return (
    <div className="mt-3 border border-brand/25 bg-brand/[0.04] p-2.5">
      <div className="flex items-start gap-2">
        <Sparkles className="mt-0.5 h-3.5 w-3.5 shrink-0 text-brand" />
        <div className="min-w-0 flex-1">
          <div className="flex items-center justify-between gap-2">
            <p className="font-mono text-[10px] uppercase tracking-[0.1em] text-brand">
              {t(`candidates.${suggestion.mode}.title`)}
            </p>
            <span className="font-mono text-[9px] text-ink/25" title={suggestion.baseSpecHash}>
              {suggestion.baseSpecHash.slice(0, 8)}
            </span>
          </div>
          <p className="mt-0.5 text-xs leading-5 text-ink/55">{t("candidates.hint")}</p>
        </div>
      </div>

      {stale && (
        <p role="alert" className="mt-2 border border-amber-500/25 bg-amber-500/[0.06] px-2 py-1.5 text-[11px] leading-4 text-amber-300">
          {t("candidates.stale")}
        </p>
      )}

      {(!stale || hasAppliedCandidate) && (
        <div className="mt-2.5 grid grid-cols-2 gap-2">
          {baselinePreview && (
            <figure className="min-w-0 border border-brand/30 bg-instrument-raised p-1.5">
              <figcaption className="mb-1.5 truncate font-mono text-[9px] uppercase tracking-wider text-brand/70">
                {t("candidates.baseline")}
              </figcaption>
              <div className="aspect-square overflow-hidden bg-black">
                {/* The Studio owns this same-origin/object URL; this card never persists it. */}
                {/* eslint-disable-next-line @next/next/no-img-element */}
                <img
                  className="h-full w-full object-contain"
                  src={baselinePreview}
                  alt={t("candidates.baselineAlt")}
                />
              </div>
            </figure>
          )}
          {suggestion.candidates.map((candidate) => {
            const candidatePreview = previews[candidate.id];
            return (
              <figure key={`preview:${candidate.id}`} className="min-w-0 border border-instrument-rule bg-instrument-raised p-1.5">
                <figcaption className="mb-1.5 flex min-w-0 items-center justify-between gap-1.5">
                  <span className="truncate text-[10px] font-medium text-ink/70" title={candidate.label}>
                    {candidate.label}
                  </span>
                  <span className="shrink-0 font-mono text-[8px] uppercase text-ink/30">{candidate.id}</span>
                </figcaption>
                <div className="flex aspect-square items-center justify-center overflow-hidden bg-black/90">
                  {candidatePreview?.status === "ready" && (
                    // This object URL exists only for the life of this card.
                    // eslint-disable-next-line @next/next/no-img-element
                    <img
                      className="h-full w-full object-contain"
                      src={candidatePreview.url}
                      alt={t("candidates.previewAlt", { label: candidate.label })}
                    />
                  )}
                  {candidatePreview?.status === "loading" && (
                    <span className="flex px-2 text-center text-[9px] leading-3 text-ink/45">
                      <Sparkles className="mr-1 h-3 w-3 shrink-0 animate-pulse" />{t("candidates.previewing")}
                    </span>
                  )}
                  {candidatePreview?.status === "error" && (
                    <span className="px-2 text-center text-[9px] leading-3 text-red-400">{t("candidates.previewFailed")}</span>
                  )}
                  {!candidatePreview && (
                    <span className="px-2 text-center text-[9px] leading-3 text-ink/35">{t("candidates.verificationPending")}</span>
                  )}
                </div>
              </figure>
            );
          })}
        </div>
      )}

      <div className="mt-2.5 grid grid-cols-1 gap-2 min-[420px]:grid-cols-2">
        {suggestion.candidates.map((candidate) => {
          const applicationKey = `${messageId}:${candidate.id}`;
          const record = applied[applicationKey];
          const baseSpec = record?.previousSpec ?? suggestion.baseSpec ?? currentSpec;
          const changes = Object.entries(candidate.patch);
          const candidatePreview = previews[candidate.id];
          const verification = candidatePreview?.status === "ready"
            ? t("candidates.verificationReady")
            : candidatePreview?.status === "error"
              ? t("candidates.verificationFailed")
              : candidate.verification === "pending"
                ? t("candidates.verificationPending")
                : displayValue(candidate.verification);
          return (
            <article key={candidate.id} className="flex min-w-0 flex-col border border-instrument-rule bg-instrument-raised p-2.5">
              <div className="flex items-start justify-between gap-2">
                <div className="min-w-0">
                  <p className="text-xs font-medium text-ink/80">{candidate.label}</p>
                  {candidate.reason && <p className="mt-1 text-[11px] leading-4 text-ink/50">{candidate.reason}</p>}
                </div>
                <span className="shrink-0 font-mono text-[9px] uppercase text-ink/30">{candidate.id}</span>
              </div>

              <dl className="mt-2 space-y-1 border-y border-instrument-rule py-2 font-mono text-[10px]">
                {changes.map(([key, value]) => (
                  <div key={key}>
                    <dt className="truncate text-ink/40" title={key}>{key}</dt>
                    <dd className="mt-0.5 min-w-0 break-all leading-4 text-ink/65">
                      <span className="text-red-400/60 line-through">{displayValue(baseSpec[key as keyof FractalSpec])}</span>
                      <span className="mx-1 text-ink/25">→</span>
                      <span className="text-brand">{displayValue(value)}</span>
                    </dd>
                  </div>
                ))}
              </dl>

              {verification !== "—" && verification !== "null" && (
                <p className="mt-2 text-[10px] leading-4 text-ink/40">
                  <span className="mr-1 text-ink/55">{t("candidates.verification")}</span>
                  {verification}
                </p>
              )}

              <div className="mt-auto flex justify-end pt-2">
                {record ? (
                  <Button type="button" size="sm" variant="outline" loading={record.busy} onClick={() => void onUndo(applicationKey)}>
                    <Undo2 className="h-3 w-3" />{t("suggestion.undo")}
                  </Button>
                ) : (
                  <Button
                    type="button"
                    size="sm"
                    variant="fractal"
                    disabled={disabled || stale || candidatePreview?.status !== "ready" || changes.length === 0}
                    onClick={() => void onApply(applicationKey, { patch: candidate.patch, reason: candidate.reason })}
                  >
                    <Check className="h-3 w-3" />{t("candidates.apply")}
                  </Button>
                )}
              </div>
            </article>
          );
        })}
      </div>

      {!previewDimensions && !stale && (
        <p role="alert" className="mt-2.5 border border-amber-500/25 bg-amber-500/[0.06] px-2 py-1.5 text-[11px] leading-4 text-amber-300">
          {t("candidates.aspectUnsupported")}
        </p>
      )}

      {!everyPreviewReady && !stale && previewDimensions && (
        <div className="mt-2.5 flex items-center justify-between gap-2">
          <span className="min-w-0 text-[10px] leading-4 text-ink/35">{t("candidates.previewNotice")}</span>
          <Button
            type="button"
            size="sm"
            variant="outline"
            className="shrink-0"
            loading={previewing}
            disabled={disabled}
            onClick={() => void generatePreviews()}
          >
            <ImageIcon className="h-3 w-3" />{t("candidates.generatePreviews")}
          </Button>
        </div>
      )}
      {previewError && <p role="alert" className="mt-2 text-[10px] text-red-400">{previewError}</p>}
    </div>
  );
}

function SuggestionCard({
  messageId,
  suggestion,
  currentSpec,
  currentContextSignature,
  applied,
  disabled = false,
  onApply,
  onUndo,
}: SuggestionCardProps) {
  const t = useTranslations("aiAssistant");
  const changes = Object.entries(suggestion.patch);
  const baseContextSignature = suggestionContextSignature(suggestion);
  const stale = baseContextSignature === null || currentContextSignature !== baseContextSignature;
  const baseSpec = applied?.previousSpec ?? suggestion.baseSpec ?? currentSpec;
  return (
    <div className="mt-3 border border-brand/25 bg-brand/[0.04] p-2.5">
      <div className="mb-2 flex items-start gap-2">
        <Sparkles className="mt-0.5 h-3.5 w-3.5 shrink-0 text-brand" />
        <div className="min-w-0">
          <p className="font-mono text-[10px] uppercase tracking-[0.1em] text-brand">{t("suggestion.title")}</p>
          {suggestion.reason && <p className="mt-0.5 text-xs leading-5 text-ink/55">{suggestion.reason}</p>}
        </div>
      </div>
      {stale && !applied && (
        <p role="alert" className="mb-2 border border-amber-500/25 bg-amber-500/[0.06] px-2 py-1.5 text-[11px] leading-4 text-amber-300">
          {t("suggestion.stale")}
        </p>
      )}
      <dl className="space-y-1 border-y border-instrument-rule py-2 font-mono text-[10px]">
        {changes.map(([key, value]) => (
          <div key={key} className="grid grid-cols-[minmax(5rem,0.7fr)_1fr] gap-2">
            <dt className="truncate text-ink/45" title={key}>{key}</dt>
            <dd className="min-w-0 break-all text-ink/65">
              <span className="text-red-400/60 line-through">{displayValue(baseSpec[key as keyof FractalSpec])}</span>
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
          <Button type="button" size="sm" variant="fractal" disabled={disabled || stale || changes.length === 0} onClick={() => void onApply(messageId, suggestion)}>
            <Check className="h-3 w-3" />{t("suggestion.apply")}
          </Button>
        )}
      </div>
    </div>
  );
}
