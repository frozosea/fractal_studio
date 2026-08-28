"use client";

/** Browser client for the public Platform API. Compute stays worker-only. */

export type Role = "admin" | "creator" | "finance_operator" | string;

export interface PlatformUser {
  id: string;
  email: string;
  roles: Role[];
  status: "active" | "disabled";
  member?: boolean;
  creatorProfile?: { handle: string; displayName: string } | null;
}

export interface Page<T> {
  data: T[];
  page: { nextCursor: string | null };
}

export interface Recipe {
  id: string;
  ownerId: string;
  canonicalSpec: FractalSpec;
  specHash: string;
  rendererVersion: string;
  createdAt: string;
}

export interface FractalSpec {
  version: 1;
  centerRe?: number;
  centerIm?: number;
  centerReStr?: string;
  centerImStr?: string;
  scale?: number;
  iterations?: number;
  variant?: string;
  colorMap?: string | null;
  metric?: "escape" | "min_abs" | "max_abs" | "envelope" | "min_pairwise_dist" | "mandel_ship_agree";
  smooth?: boolean;
  colorMode?: "direct" | "eq_full" | "eq_center";
  cyclesPerOctave?: number;
  rotationDeg?: number;
  pairwiseCap?: number;
  colorProgram?: {
    schemaVersion?: 1;
    type?: "gradient";
    interpolation?: "rgb";
    wrap?: "clamp" | "repeat" | "mirror";
    cycles?: number;
    phase?: number;
    interiorColor?: string;
    invalidColor?: string;
    stops: Array<{ at: number; color: string }>;
  } | null;
  orbitProgram?: OrbitProgram | null;
  julia?: boolean;
  juliaRe?: number;
  juliaIm?: number;
  bailout?: number;
  engine?: "auto" | "cpu" | "cuda" | "openmp" | "avx2" | "avx512" | "hybrid";
  scalarType?: "auto" | "float" | "double" | "long_double" | "fp32" | "fp64" | "fx64" | "fp80" | "fp128";
  transitionMode?: "off" | "pair" | "multi";
  transitionThetaMilliDeg?: number;
  transitionFrom?: string;
  transitionTo?: string;
  transitionLegs?: Array<{ variant: string; weight: number }>;
}

export type FormulaDefinition =
  | { type: "builtin"; id: string }
  | {
      type: "dsl";
      source: string;
      parameters?: Record<string, number | { re: number; im: number }>;
    };

export type FormulaProgram = { type: "formula"; formula: FormulaDefinition };

export type OrbitProgram =
  | FormulaProgram
  | {
      type: "sequence";
      repeat: true;
      steps: Array<{ span: number; program: FormulaProgram }>;
    };

export interface StudioCapabilities {
  rendererVersion?: string;
  metrics: string[];
  engines: string[];
  scalars: string[];
  colorMaps: string[];
  colorModes: string[];
  variants: string[];
  axisTransitionVariants: string[];
  imageKinds: {
    map: StudioImageKindCapabilities;
    transition: StudioImageKindCapabilities;
  };
  orbitPrograms: Record<string, boolean>;
  customGradient: { enabled: boolean; maxStops: number; kinds: string[] };
}

export interface StudioImageKindCapabilities {
  enabled: boolean;
  metrics: string[];
  engines: string[];
  scalars: string[];
  orbitProgram: boolean;
}

export interface PreviewJob {
  id: string;
  status: "queued" | "deferred" | "rendering" | "completed" | "stale";
}

/** Free PNG exports left. `limit`/`remaining` are null for members: unmetered. */
export interface ExportAllowance {
  member: boolean;
  limit: number | null;
  used: number;
  remaining: number | null;
}

export interface AIConversation {
  id: string;
  title: string;
  optimizationConsent: boolean;
  createdAt: string;
  updatedAt: string;
}

export type AIStudioMode = "map" | "julia" | "transitionPair" | "transitionMulti" | "formula" | "sequence";

export interface AIStudioSuggestionBaseline {
  /** Optional only for safe handling of suggestions saved before the baseline protocol existed. */
  baseSpec?: FractalSpec;
  baseMode?: AIStudioMode;
  baseOutput?: { width: number; height: number };
}

export interface AIStudioPatchSuggestion extends AIStudioSuggestionBaseline {
  patch: Partial<FractalSpec>;
  reason: string;
}

export type AIAssistantMode = "chat" | "location" | "color" | "composition";

export interface AIStudioCandidate {
  id: string;
  label: string;
  patch: Partial<FractalSpec>;
  reason: string;
  verification?: unknown;
}

export interface AIStudioCandidateSet extends AIStudioSuggestionBaseline {
  kind: "candidate_set";
  mode: Exclude<AIAssistantMode, "chat">;
  baseSpecHash: string;
  candidates: AIStudioCandidate[];
}

export type AIStudioSuggestion = AIStudioPatchSuggestion | AIStudioCandidateSet;

export interface AIMessageFeedback {
  rating: -1 | 1;
  consent: boolean;
}

export interface AIMessage {
  id: string;
  conversationId?: string;
  role: "user" | "assistant";
  content: string;
  suggestion: AIStudioSuggestion | null;
  feedback?: AIMessageFeedback | -1 | 1 | null;
  status?: "completed" | "partial" | "retrying" | null;
  createdAt: string;
}

/** Free AI replies left. `limit`/`remaining` are null for members: unmetered. */
export interface AIAllowance {
  member: boolean;
  limit: number | null;
  used: number;
  remaining: number | null;
  enabled?: boolean;
}

export interface AIUsage {
  promptTokens?: number;
  completionTokens?: number;
  totalTokens?: number;
}

export interface AIListingCopyCandidate {
  title: string;
  description: string;
  tags: string[];
}

export interface AIListingCopyResponse {
  requestId: string;
  candidates: AIListingCopyCandidate[];
  allowance: AIAllowance;
}

export type AIStreamEvent =
  | { type: "message"; messageId: string; message?: AIMessage }
  | { type: "delta"; content: string }
  | { type: "suggestion"; suggestion: AIStudioSuggestion }
  | { type: "usage"; usage: AIUsage }
  | { type: "done"; messageId: string; message?: AIMessage; allowance?: AIAllowance; partial?: boolean; replayed?: boolean }
  | { type: "error"; code: string; message: string };

export interface AIMessageInput {
  text: string;
  context: {
    spec: FractalSpec;
    mode: string;
    output: { width: number; height: number; preset?: string };
    capabilities: StudioCapabilities;
  };
  /** Force the provider's sole Studio-patch tool for an explicit suggestion action. */
  requestPatch?: boolean;
  assistantMode?: AIAssistantMode;
  image?: Blob | null;
  signal?: AbortSignal;
  /** Reuse this key when retrying a request whose outcome is unknown. */
  idempotencyKey?: string;
}

export interface RenderJob {
  id: string;
  recipeId: string;
  status: string;
  progressPercent: number;
  assetId?: string | null;
  errorCode?: string | null;
  createdAt: string;
}

export interface Asset {
  id: string;
  recipeId: string;
  mediaType: "image" | "video" | "mesh";
  status: "processing" | "ready" | "failed" | "deleted";
  visibility: "private" | "hidden";
  /** Live listing for this asset, if any; null once withdrawn. */
  listingStatus?: "draft" | "published" | "unpublished" | null;
  derivativeStatus: "pending" | "ready" | "failed";
  derivativeErrorCode?: string | null;
  preview?: { thumbnailUrl?: string | null; watermarkedPreviewUrl?: string | null; videoPosterUrl?: string | null } | null;
  createdAt: string;
  files: Array<{ purpose: string; mediaType: string; sizeBytes: number }>;
}

/** Intrinsic properties of the render, denormalized onto the listing. */
export interface RenderMeta {
  variant?: string | null;
  iterations?: number | null;
  width?: number | null;
  height?: number | null;
  colorMap?: string | null;
  colorMode?: string | null;
  viewScale?: number | null;
}

/** Facet values present in the published catalogue, with listing counts. */
export type FacetName = "variant" | "colorMap" | "depth" | "resolution" | "creator";
export interface FacetCount {
  facet: FacetName;
  value: string;
  count: number;
}

/** Public creator profile shown on a shareable creator page. */
export interface CreatorProfile {
  handle: string;
  displayName: string;
  publishedCount: number;
}

export interface ExploreQuery {
  q?: string;
  variant?: string | null;
  colorMap?: string | null;
  depth?: string | null;
  resolution?: string | null;
  creator?: string | null;
  cursor?: string | null;
  limit?: number;
}

export interface Listing {
  id: string;
  assetId: string;
  creator: { id: string; handle: string; displayName: string };
  status: "draft" | "published" | "unpublished";
  title: string;
  description: string;
  tags: string[];
  price: string;
  currency: "CNY";
  publishedAt?: string | null;
  preview?: { mediaType?: "image" | "video" | "mesh"; thumbnailUrl?: string | null; watermarkedPreviewUrl?: string | null; videoPosterUrl?: string | null } | null;
  render?: RenderMeta | null;
  licenceOffer: { id: string; code: string; termsVersion: string; terms: Record<string, unknown> };
}

export interface Order {
  id: string;
  status: "pending_payment" | "fulfilled" | "closed" | "payment_exception";
  amount: string;
  currency: "CNY";
  paidAt?: string | null;
  createdAt: string;
  outTradeNo?: string | null;
  items: Array<{
    assetId: string;
    listingId: string;
    price: string;
    /** Immutable listing snapshot taken when the order was placed. */
    snapshot?: Record<string, unknown> | null;
  }>;
}

export interface PayoutRequest {
  id: string;
  amount: string;
  currency: string;
  status: "pending" | "paid" | "rejected" | "cancelled";
  createdAt: string;
  paidAt?: string | null;
  rejectionReason?: string | null;
}

export interface CreatorBalance {
  availableAmount: string;
  reservedAmount: string;
  currency: "CNY";
}

export interface InternalPayoutRequest extends PayoutRequest {
  creator: { email?: string | null; handle?: string | null; displayName?: string | null };
  qrUrl?: string | null;
  qrExpiresAt?: string | null;
  externalReference?: string | null;
  operator?: { id: string; email: string } | null;
}

export interface AdminStatistics {
  generatedAt: string;
  users: {
    total: number;
    active: number;
    disabled: number;
    creators: number;
    members: number;
    admins: number;
    newLast30Days: number;
  };
  market: {
    listings: number;
    published: number;
    draft: number;
    unpublished: number;
    archived: number;
    readyAssets: number;
    favorites: number;
  };
  commerce: {
    orders: number;
    fulfilled: number;
    pendingPayment: number;
    paymentExceptions: number;
    marketplaceGrossCny: string;
    membershipRevenueCny: string;
    creatorRevenueCny: string;
    platformRevenueCny: string;
  };
  compute: {
    renderJobs: number;
    active: number;
    completed: number;
    failed: number;
  };
}

export interface AdminComputeNode {
  nodeKey: string;
  state: "active" | "draining" | "offline" | "disabled";
  healthy: boolean;
  maxDurableSlots: number;
  usedDurableSlots: number;
  maxPreviewSlots: number;
  usedPreviewSlots: number;
  rendererVersion?: string | null;
  cpuAllocation?: { usedSlots: number; maxSlots: number; usedPreviewSlots: number; maxPreviewSlots: number } | null;
  gpuAllocation?: { usedSlots: number; maxSlots: number; usedPreviewSlots: number; maxPreviewSlots: number } | null;
  cpu?: {
    logicalCores?: number | null;
    physicalCores?: number | null;
    openmp?: { compiled: boolean; runtime: boolean } | null;
    avx2?: { compiled: boolean; runtime: boolean } | null;
    avx512?: { compiled: boolean; runtime: boolean } | null;
  } | null;
  gpu?: {
    name?: string | null;
    runtime: boolean;
    computeCapability?: { major: number; minor: number } | null;
    totalVramBytes?: number | null;
    freeVramBytes?: number | null;
  } | null;
  lastHealthyAt?: string | null;
  lastAssignedAt?: string | null;
  capabilitiesAt?: string | null;
}

export interface AdminUser {
  id: string;
  email: string;
  status: "active" | "disabled";
  roles: Array<"admin" | "creator" | "finance_operator">;
  member: boolean;
  creatorProfile?: { handle: string; displayName: string } | null;
  assetCount: number;
  listingCount: number;
  fulfilledOrderCount: number;
  createdAt: string;
}

export interface AdminListing {
  id: string;
  assetId: string;
  creatorId: string;
  creatorEmail: string;
  creatorHandle?: string | null;
  creatorDisplayName?: string | null;
  status: "draft" | "published" | "unpublished" | "archived";
  title: string;
  description: string;
  tags: string[];
  price: string;
  currency: "CNY";
  mediaType: "image" | "video" | "mesh";
  favoriteCount: number;
  saleCount: number;
  createdAt: string;
  publishedAt?: string | null;
  preview?: Listing["preview"];
}

export class PlatformApiError extends Error {
  constructor(public readonly status: number, public readonly code: string, message: string) {
    super(message);
    this.name = "PlatformApiError";
  }
}

const baseUrl = process.env.NEXT_PUBLIC_PLATFORM_API_URL ?? "/platform";
let csrf: string | null = null;
export const PLATFORM_REQUEST_ACTIVITY_EVENT = "fractal-platform-request-activity";
const COLLECTION_CACHE_TTL_MS = 30_000;
let activeRequestCount = 0;
const collectionCache = new Map<
  string,
  { expiresAt: number; promise: Promise<Page<unknown>> }
>();

const SESSION_TOKEN_KEY = "fractal-studio:session-token";

/**
 * This tab's session token.
 *
 * sessionStorage is per-tab, unlike the cookie every tab in the window shares,
 * so holding the token here is what lets two tabs stay signed in as different
 * accounts. Once `ensureTabSession` has run this is the tab's only credential:
 * requests no longer send the cookie at all.
 */
export function sessionToken(): string | null {
  try {
    return window.sessionStorage.getItem(SESSION_TOKEN_KEY);
  } catch {
    return null;
  }
}

function storeSessionToken(token: string | null): void {
  try {
    if (token) window.sessionStorage.setItem(SESSION_TOKEN_KEY, token);
    else window.sessionStorage.removeItem(SESSION_TOKEN_KEY);
  } catch {
    /* storage disabled — this tab cannot persist a session across a reload */
  }
}

const LIVE_TABS_KEY = "fractal-studio:signed-in-tabs";
/** A tab republishes its heartbeat this often; entries go stale at 4x that. */
const TAB_HEARTBEAT_MS = 15_000;
const TAB_STALE_MS = 60_000;

let tabSession: Promise<void> | null = null;

/**
 * Identifies this tab in the signed-in registry. Lazy so the module stays
 * evaluable during server rendering.
 */
let tabId: string | null = null;
function thisTabId(): string {
  tabId ??= crypto.randomUUID();
  return tabId;
}

interface LiveTab {
  /** Last heartbeat, epoch ms. */
  at: number;
  /** Fingerprint of the session that tab holds; identifies a cloned copy. */
  token: string;
}

/** Equality tag only — never a credential, and never sent anywhere. */
function fingerprint(token: string): string {
  let hash = 0x811c9dc5;
  for (let index = 0; index < token.length; index += 1) {
    hash = Math.imul(hash ^ token.charCodeAt(index), 0x01000193) >>> 0;
  }
  return hash.toString(36);
}

function readLiveTabs(): Record<string, LiveTab> {
  try {
    const raw = window.localStorage.getItem(LIVE_TABS_KEY);
    const parsed: unknown = raw ? JSON.parse(raw) : null;
    if (!parsed || typeof parsed !== "object") return {};
    const cutoff = Date.now() - TAB_STALE_MS;
    const live: Record<string, LiveTab> = {};
    for (const [id, value] of Object.entries(parsed as Record<string, unknown>)) {
      const entry = value as Partial<LiveTab> | null;
      if (entry && typeof entry.at === "number" && entry.at > cutoff && typeof entry.token === "string") {
        live[id] = { at: entry.at, token: entry.token };
      }
    }
    return live;
  } catch {
    return {};
  }
}

function writeLiveTabs(tabs: Record<string, LiveTab>): void {
  try {
    window.localStorage.setItem(LIVE_TABS_KEY, JSON.stringify(tabs));
  } catch {
    /* storage disabled — this tab simply never advertises itself */
  }
}

let heartbeat: number | null = null;

/**
 * Advertise this tab as signed in, and keep saying so.
 *
 * localStorage rather than a live BroadcastChannel handshake on purpose: a
 * backgrounded tab that the browser has throttled or frozen answers no
 * messages, and a missed answer used to silently hand the new tab the shared
 * cookie. A written record needs nobody awake to be read.
 */
function registerSignedInTab(): void {
  const publish = () => {
    const token = sessionToken();
    if (!token) return;
    writeLiveTabs({ ...readLiveTabs(), [thisTabId()]: { at: Date.now(), token: fingerprint(token) } });
  };
  publish();
  if (heartbeat !== null) return;
  heartbeat = window.setInterval(publish, TAB_HEARTBEAT_MS);
  window.addEventListener("pagehide", unregisterSignedInTab);
}

function unregisterSignedInTab(): void {
  if (heartbeat !== null) {
    window.clearInterval(heartbeat);
    heartbeat = null;
  }
  const tabs = readLiveTabs();
  delete tabs[thisTabId()];
  writeLiveTabs(tabs);
}

/**
 * True when a live tab other than this one already holds this exact session.
 *
 * The browser copies sessionStorage into a tab opened from a link or duplicated
 * from this one, so a fresh tab can arrive already holding another tab's token.
 * The copy gives it up rather than run alongside the original.
 */
function isClonedSession(token: string): boolean {
  const tag = fingerprint(token);
  return Object.entries(readLiveTabs()).some(([id, entry]) => id !== thisTabId() && entry.token === tag);
}

/** Another tab, not this one, is signed in right now. */
function otherSignedInTabs(): boolean {
  return Object.keys(readLiveTabs()).some((id) => id !== thisTabId());
}

async function adoptCookieSession(): Promise<void> {
  try {
    const response = await fetch(`${baseUrl}/v1/auth/session-token`, {
      headers: { Accept: "application/json" },
      credentials: "include",
      cache: "no-store",
    });
    if (!response.ok) return; // no cookie session to inherit — sign-in screen
    const body = (await response.json()) as { data: { sessionToken?: string } };
    if (body.data.sessionToken) {
      storeSessionToken(body.data.sessionToken);
      registerSignedInTab();
    }
  } catch {
    /* offline or blocked — the tab starts signed out and can sign in normally */
  }
}

/**
 * Settle this tab's identity once, before any API call goes out.
 *
 * The outcome is always the same shape: the tab either holds its own bearer
 * token in sessionStorage or holds nothing and is signed out. Requests never
 * carry the shared cookie afterwards, so no tab can change what another tab is
 * signed in as — whatever happens here, that isolation holds.
 *
 * The one decision left is whether a tab that starts empty may inherit the
 * window's cookie session. It may only when no other tab is signed in; that is
 * what makes a second tab open the sign-in screen while keeping "reopen the
 * site later" working.
 */
export function ensureTabSession(): Promise<void> {
  tabSession ??= (async () => {
    const inherited = sessionToken();
    if (inherited && !isClonedSession(inherited)) {
      registerSignedInTab();
      return;
    }
    if (inherited) storeSessionToken(null);
    if (otherSignedInTabs()) return;
    await adoptCookieSession();
  })();
  return tabSession;
}

/**
 * User the cached collections belong to. Entries are keyed by identity as well
 * as path, so a cache miss — not another account's rows — is the worst case if
 * some code path forgets to reset on sign-in/sign-out.
 */
let cacheIdentity = "anonymous";

/**
 * Drop all per-session client state. Call on every sign-in, sign-out, and
 * observed identity change; pass the new user id so cached entries written
 * before the switch can never be read afterwards.
 */
export function resetPlatformClientState(userId: string | null = null): void {
  cacheIdentity = userId ?? "anonymous";
  collectionCache.clear();
  csrf = null;
}

function reportRequestActivity(change: 1 | -1): void {
  activeRequestCount = Math.max(0, activeRequestCount + change);
  window.dispatchEvent(
    new CustomEvent<{ active: number }>(PLATFORM_REQUEST_ACTIVITY_EVENT, {
      detail: { active: activeRequestCount },
    }),
  );
}

function idempotencyKey(): string {
  return crypto.randomUUID();
}

/** Headers every call needs: this tab's identity, never the shared cookie's. */
function authHeaders(base?: HeadersInit): Headers {
  const headers = new Headers(base);
  const token = sessionToken();
  if (token) headers.set("Authorization", `Bearer ${token}`);
  return headers;
}

async function csrfToken(): Promise<string> {
  if (csrf) return csrf;
  const response = await fetch(`${baseUrl}/v1/auth/csrf-token`, {
    headers: authHeaders(), credentials: "omit", cache: "no-store",
  });
  if (!response.ok) throw await asError(response);
  const body = (await response.json()) as { data: { token: string } };
  csrf = body.data.token;
  return csrf;
}

async function asError(response: Response): Promise<PlatformApiError> {
  const fallback = `Platform request failed (${response.status})`;
  try {
    const body = (await response.json()) as { error?: { code?: string; message?: string } };
    return new PlatformApiError(response.status, body.error?.code ?? "request_failed", body.error?.message ?? fallback);
  } catch {
    return new PlatformApiError(response.status, "request_failed", fallback);
  }
}

async function request<T>(
  path: string,
  init: RequestInit = {},
  options: {
    csrf?: boolean;
    idempotency?: boolean;
    idempotencyKey?: string;
    raw?: boolean;
    cookie?: boolean;
  } = {},
): Promise<T> {
  reportRequestActivity(1);
  try {
    await ensureTabSession();
    const headers = authHeaders(init.headers);
    headers.set("Accept", "application/json");
    const method = (init.method ?? "GET").toUpperCase();
    if (init.body && !(init.body instanceof FormData)) headers.set("Content-Type", "application/json");
    if (options.idempotencyKey) headers.set("Idempotency-Key", options.idempotencyKey);
    else if (options.idempotency) headers.set("Idempotency-Key", idempotencyKey());
    if (options.csrf) headers.set("X-CSRF-Token", await csrfToken());
    const response = await fetch(`${baseUrl}${path}`, {
      // Only sign-in and sign-out touch the shared cookie, so that reopening
      // the site later restores the account signed in last. Every other call is
      // bearer-only and cannot be redirected by another tab.
      ...init, method, headers, credentials: options.cookie ? "include" : "omit", cache: "no-store",
    });
    if (!response.ok) throw await asError(response);
    // Session rotation (e.g. creator-profile) reissues the token; a bearer tab
    // has to adopt it or its stored token is the revoked one.
    const rotated = response.headers.get("X-Session-Token");
    if (rotated) storeSessionToken(rotated);
    if (method !== "GET") collectionCache.clear();
    if (options.raw) return response as T;
    if (response.status === 204) return undefined as T;
    const body = (await response.json()) as { data: T };
    return body.data;
  } finally {
    reportRequestActivity(-1);
  }
}

function objectValue(value: unknown): Record<string, unknown> | null {
  return value !== null && typeof value === "object" && !Array.isArray(value)
    ? value as Record<string, unknown>
    : null;
}

function normalizeStudioBaseline(value: Record<string, unknown>): AIStudioSuggestionBaseline {
  const baseSpec = objectValue(value.baseSpec ?? value.base_spec);
  const baseMode = value.baseMode ?? value.base_mode;
  const rawOutput = objectValue(value.baseOutput ?? value.base_output);
  const width = rawOutput?.width;
  const height = rawOutput?.height;
  const validMode = ["map", "julia", "transitionPair", "transitionMulti", "formula", "sequence"]
    .includes(String(baseMode));
  const validOutput = typeof width === "number" && Number.isInteger(width) && width >= 64 && width <= 4096
    && typeof height === "number" && Number.isInteger(height) && height >= 64 && height <= 4096;
  if (!baseSpec || !validMode || !validOutput) return {};
  return {
    baseSpec: baseSpec as unknown as FractalSpec,
    baseMode: baseMode as AIStudioMode,
    baseOutput: { width, height },
  };
}

function normalizeStudioSuggestion(value: unknown): AIStudioSuggestion | null {
  const outer = objectValue(value);
  if (!outer) return null;
  const candidate = objectValue(outer.candidateSet ?? outer.candidate_set) ?? outer;
  const patch = objectValue(candidate.patch);
  if (patch) {
    return {
      patch: patch as Partial<FractalSpec>,
      reason: typeof candidate.reason === "string" ? candidate.reason : "",
      ...normalizeStudioBaseline(candidate),
    };
  }
  const mode = candidate.mode;
  const baseSpecHash = candidate.baseSpecHash ?? candidate.base_spec_hash;
  const baseSpec = objectValue(candidate.baseSpec ?? candidate.base_spec);
  if (
    candidate.kind !== "candidate_set"
    || !["location", "color", "composition"].includes(String(mode))
    || typeof baseSpecHash !== "string"
    || !baseSpec
    || !Array.isArray(candidate.candidates)
  ) return null;
  const candidates = candidate.candidates.flatMap((raw, index) => {
    const item = objectValue(raw);
    const itemPatch = objectValue(item?.patch);
    if (!item || !itemPatch) return [];
    return [{
      id: typeof item.id === "string" ? item.id : `candidate-${index + 1}`,
      label: typeof item.label === "string" ? item.label : `Candidate ${index + 1}`,
      patch: itemPatch as Partial<FractalSpec>,
      reason: typeof item.reason === "string" ? item.reason : "",
      ...(item.verification === undefined ? {} : { verification: item.verification }),
    } satisfies AIStudioCandidate];
  });
  if (candidates.length === 0) return null;
  return {
    kind: "candidate_set",
    mode: mode as AIStudioCandidateSet["mode"],
    baseSpecHash,
    ...normalizeStudioBaseline(candidate),
    candidates,
  };
}

function normalizeAIMessage(message: AIMessage): AIMessage {
  return {
    ...message,
    suggestion: message.suggestion ? normalizeStudioSuggestion(message.suggestion) : null,
  };
}

function normalizeAIStreamEvent(eventName: string, rawData: string): AIStreamEvent | null {
  let parsed: unknown;
  try {
    parsed = JSON.parse(rawData);
  } catch {
    parsed = rawData;
  }

  const outer = objectValue(parsed);
  const type = eventName === "message" && typeof outer?.type === "string"
    ? outer.type
    : eventName;
  const wrapped = outer && "data" in outer ? outer.data : parsed;
  const payload = objectValue(wrapped);

  if (type === "delta") {
    const content = typeof wrapped === "string"
      ? wrapped
      : typeof payload?.content === "string"
        ? payload.content
        : typeof payload?.delta === "string" ? payload.delta : "";
    return content ? { type, content } : null;
  }
  if (type === "suggestion") {
    const candidate = objectValue(payload?.suggestion) ?? payload;
    const suggestion = normalizeStudioSuggestion(candidate);
    return suggestion ? { type, suggestion } : null;
  }
  if (type === "usage") {
    const candidate = objectValue(payload?.usage) ?? payload;
    if (!candidate) return null;
    const number = (camel: string, snake: string): number | undefined => {
      const value = candidate[camel] ?? candidate[snake];
      return typeof value === "number" ? value : undefined;
    };
    return {
      type,
      usage: {
        promptTokens: number("promptTokens", "prompt_tokens"),
        completionTokens: number("completionTokens", "completion_tokens"),
        totalTokens: number("totalTokens", "total_tokens"),
      },
    };
  }
  if (type === "error") {
    return {
      type,
      code: typeof payload?.code === "string" ? payload.code : "AI_STREAM_FAILED",
      message: typeof payload?.message === "string" ? payload.message : "AI response failed",
    };
  }
  if (type === "message" || type === "done") {
    const rawMessage = objectValue(payload?.message) as unknown as AIMessage | null;
    const message = rawMessage ? normalizeAIMessage(rawMessage) : null;
    const messageId = typeof wrapped === "string"
      ? wrapped
      : typeof payload?.messageId === "string"
        ? payload.messageId
        : typeof payload?.id === "string" ? payload.id : message?.id ?? "";
    if (!messageId) return null;
    if (type === "message") return { type, messageId, ...(message ? { message } : {}) };
    const allowance = objectValue(payload?.allowance) as unknown as AIAllowance | null;
    return {
      type,
      messageId,
      ...(message ? { message } : {}),
      ...(allowance ? { allowance } : {}),
      ...(payload?.partial === true ? { partial: true } : {}),
      ...(payload?.replayed === true ? { replayed: true } : {}),
    };
  }
  return null;
}

async function streamAIMessage(
  conversationId: string,
  input: AIMessageInput,
  onEvent: (event: AIStreamEvent) => void | Promise<void>,
): Promise<void> {
  reportRequestActivity(1);
  try {
    await ensureTabSession();
    const body = new FormData();
    body.set("text", input.text);
    body.set("context", JSON.stringify(input.context));
    body.set("requestPatch", String(input.requestPatch ?? false));
    body.set("assistantMode", input.assistantMode ?? "chat");
    if (input.image) {
      const extension = input.image.type === "image/png" ? "png" : "jpg";
      body.set("image", input.image, `studio-preview.${extension}`);
    }

    const headers = authHeaders({ Accept: "text/event-stream" });
    headers.set("X-CSRF-Token", await csrfToken());
    headers.set("Idempotency-Key", input.idempotencyKey ?? idempotencyKey());
    const response = await fetch(
      `${baseUrl}/v1/ai/conversations/${encodeURIComponent(conversationId)}/messages`,
      {
        method: "POST",
        headers,
        body,
        signal: input.signal,
        credentials: "omit",
        cache: "no-store",
      },
    );
    if (!response.ok) throw await asError(response);
    const rotated = response.headers.get("X-Session-Token");
    if (rotated) storeSessionToken(rotated);
    collectionCache.clear();
    if (!response.body) throw new PlatformApiError(502, "AI_STREAM_FAILED", "AI response stream is empty");

    const reader = response.body.getReader();
    const decoder = new TextDecoder();
    let buffer = "";
    let eventName = "message";
    let dataLines: string[] = [];
    const dispatch = async () => {
      if (dataLines.length === 0) {
        eventName = "message";
        return;
      }
      const event = normalizeAIStreamEvent(eventName, dataLines.join("\n"));
      eventName = "message";
      dataLines = [];
      if (event) await onEvent(event);
    };
    const consumeLines = async (flush: boolean) => {
      for (;;) {
        const newline = buffer.indexOf("\n");
        if (newline < 0) break;
        const line = buffer.slice(0, newline).replace(/\r$/, "");
        buffer = buffer.slice(newline + 1);
        if (!line) await dispatch();
        else if (line.startsWith("event:")) eventName = line.slice(6).trim() || "message";
        else if (line.startsWith("data:")) dataLines.push(line.slice(5).replace(/^ /, ""));
      }
      if (flush) {
        const line = buffer.replace(/\r$/, "");
        buffer = "";
        if (line.startsWith("event:")) eventName = line.slice(6).trim() || "message";
        else if (line.startsWith("data:")) dataLines.push(line.slice(5).replace(/^ /, ""));
        await dispatch();
      }
    };

    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      buffer += decoder.decode(value, { stream: true });
      await consumeLines(false);
    }
    buffer += decoder.decode();
    await consumeLines(true);
  } finally {
    reportRequestActivity(-1);
  }
}

function collection<T>(path: string, options: { fresh?: boolean } = {}): Promise<Page<T>> {
  const key = `${cacheIdentity}::${path}`;
  const cached = collectionCache.get(key);
  if (!options.fresh && cached && cached.expiresAt > Date.now()) return cached.promise as Promise<Page<T>>;

  reportRequestActivity(1);
  const pending = (async (): Promise<Page<T>> => {
    try {
      await ensureTabSession();
      const response = await fetch(`${baseUrl}${path}`, {
        headers: authHeaders({ Accept: "application/json" }),
        credentials: "omit",
        cache: "no-store",
      });
      if (!response.ok) throw await asError(response);
      return response.json() as Promise<Page<T>>;
    } finally {
      reportRequestActivity(-1);
    }
  })();
  const sharedPending = pending as Promise<Page<unknown>>;
  collectionCache.set(key, { expiresAt: Date.now() + COLLECTION_CACHE_TTL_MS, promise: sharedPending });
  void pending.catch(() => {
    if (collectionCache.get(key)?.promise === sharedPending) collectionCache.delete(key);
  });
  return pending;
}

/**
 * Bind a sign-in to this tab: keep the token, and drop it from the user object
 * so it never reaches React state or a cache.
 */
function adoptSession(user: PlatformUser & { sessionToken?: string }): PlatformUser {
  const { sessionToken: token, ...rest } = user;
  if (token) {
    storeSessionToken(token);
    registerSignedInTab();
  }
  csrf = null;
  return rest;
}

function json(body: unknown): string {
  return JSON.stringify(body);
}

export const platform = {
  auth: {
    register: async (email: string, password: string) =>
      adoptSession(await request<PlatformUser>("/v1/auth/register", { method: "POST", body: json({ email, password }) }, { cookie: true })),
    login: async (email: string, password: string) =>
      adoptSession(await request<PlatformUser>("/v1/auth/login", { method: "POST", body: json({ email, password }) }, { cookie: true })),
    me: () => request<PlatformUser>("/v1/me"),
    logout: async () => {
      await request<void>("/v1/auth/logout", { method: "POST" }, { csrf: true, cookie: true });
      storeSessionToken(null);
      unregisterSignedInTab();
      csrf = null;
    },
    creatorProfile: (handle: string, displayName: string) => request<PlatformUser>("/v1/me/creator-profile", { method: "PATCH", body: json({ handle, displayName }) }, { csrf: true, idempotency: true }),
  },
  ai: {
    conversations: () => request<AIConversation[]>("/v1/ai/conversations"),
    createConversation: (title = "新对话") => request<AIConversation>(
      "/v1/ai/conversations",
      { method: "POST", body: json({ title }) },
      { csrf: true, idempotency: true },
    ),
    updateConversation: (
      conversationId: string,
      body: Partial<Pick<AIConversation, "title" | "optimizationConsent">>,
    ) => request<AIConversation>(
      `/v1/ai/conversations/${encodeURIComponent(conversationId)}`,
      { method: "PATCH", body: json(body) },
      { csrf: true, idempotency: true },
    ),
    deleteConversation: (conversationId: string) => request<void>(
      `/v1/ai/conversations/${encodeURIComponent(conversationId)}`,
      { method: "DELETE" },
      { csrf: true, idempotency: true },
    ),
    deleteAllConversations: () => request<void>(
      "/v1/me/ai-conversations",
      { method: "DELETE" },
      { csrf: true, idempotency: true },
    ),
    messages: async (conversationId: string) => (
      await request<AIMessage[]>(
        `/v1/ai/conversations/${encodeURIComponent(conversationId)}/messages`,
      )
    ).map(normalizeAIMessage),
    allowance: () => request<AIAllowance>("/v1/me/ai-allowance"),
    listingCopy: (
      body: {
        listingId: string;
        locale: string;
        sourceRequestId?: string;
        instruction?: string;
      },
      requestKey: string,
      signal?: AbortSignal,
    ) => request<AIListingCopyResponse>(
      "/v1/ai/listing-copy",
      { method: "POST", body: json(body), signal },
      { csrf: true, idempotencyKey: requestKey },
    ),
    feedback: (messageId: string, rating: -1 | 1, consent: boolean) => request<AIMessageFeedback>(
      `/v1/ai/messages/${encodeURIComponent(messageId)}/feedback`,
      { method: "POST", body: json({ rating, consent }) },
      { csrf: true, idempotency: true },
    ),
    streamMessage: streamAIMessage,
  },
  studio: {
    preview: async (
      canonicalSpec: FractalSpec,
      width = 512,
      height = 512,
      signal?: AbortSignal,
      channel: "main" | "julia_picker" | "ai_candidate" = "main",
      onStatus?: (status: PreviewJob["status"]) => void,
    ): Promise<Blob> => {
      const job = await request<PreviewJob>("/v1/studio/preview-jobs", { method: "POST", body: json({ canonicalSpec, width, height, channel }), signal }, { csrf: true });
      onStatus?.(job.status);
      for (;;) {
        if (signal?.aborted) throw new DOMException("Preview request aborted", "AbortError");
        const current = job.status === "completed" ? job : await request<PreviewJob>(`/v1/studio/preview-jobs/${job.id}`, { signal });
        onStatus?.(current.status);
        if (current.status === "completed") {
          const response = await request<Response>(`/v1/studio/preview-jobs/${job.id}/image`, { signal }, { raw: true });
          return response.blob();
        }
        if (current.status === "stale") throw new DOMException("Preview superseded", "AbortError");
        await new Promise<void>((resolve, reject) => {
          const finish = () => {
            signal?.removeEventListener("abort", onAbort);
            resolve();
          };
          const timer = window.setTimeout(finish, 250);
          const onAbort = () => {
            window.clearTimeout(timer);
            reject(new DOMException("Preview request aborted", "AbortError"));
          };
          signal?.addEventListener("abort", onAbort, { once: true });
        });
      }
    },
    capabilities: () => request<StudioCapabilities>("/v1/studio/capabilities"),
    exportAllowance: () => request<ExportAllowance>("/v1/me/export-allowance"),
    createRecipe: (canonicalSpec: FractalSpec) => request<Recipe>("/v1/recipes", { method: "POST", body: json({ canonicalSpec }) }, { csrf: true, idempotency: true }),
    recipes: () => collection<Recipe>("/v1/me/recipes"),
    createRender: (recipeId: string, output: Record<string, unknown>) => request<RenderJob>("/v1/render-jobs", { method: "POST", body: json({ recipeId, output }) }, { csrf: true, idempotency: true }),
    job: (jobId: string) => request<RenderJob>(`/v1/render-jobs/${jobId}`),
    cancel: (jobId: string) => request<RenderJob>(`/v1/render-jobs/${jobId}/cancel`, { method: "POST" }, { csrf: true, idempotency: true }),
  },
  assets: {
    list: (visibility?: "private" | "hidden") => collection<Asset>(`/v1/me/assets?limit=48${visibility ? `&visibility=${visibility}` : ""}`),
    get: (assetId: string) => request<Asset>(`/v1/me/assets/${assetId}`),
    setVisibility: (assetId: string, visibility: "private" | "hidden") => request<Asset>(`/v1/me/assets/${assetId}`, { method: "PATCH", body: json({ visibility }) }, { csrf: true, idempotency: true }),
    remove: (assetId: string) => request<void>(`/v1/me/assets/${assetId}`, { method: "DELETE" }, { csrf: true, idempotency: true }),
    downloadUrl: (assetId: string) => request<{ url: string; expiresAt: string }>(`/v1/assets/${assetId}/download-url`, { method: "POST" }, { csrf: true }),
  },
  marketplace: {
    // An options object rather than positional arguments: the catalogue now has
    // four facets on top of the text query, and `collection()` caches by URL, so
    // a stable parameter order matters.
    explore: ({ q, variant, colorMap, depth, resolution, creator, cursor, limit = 12 }: ExploreQuery = {}) => {
      const params = new URLSearchParams({ limit: String(limit) });
      for (const [key, value] of [
        ["q", q], ["variant", variant], ["colorMap", colorMap],
        ["depth", depth], ["resolution", resolution], ["creator", creator], ["cursor", cursor],
      ] as const) {
        if (value) params.set(key, value);
      }
      return collection<Listing>(`/v1/explore?${params.toString()}`);
    },
    facets: () => collection<FacetCount>("/v1/explore/facets"),
    creator: (handle: string) => request<CreatorProfile>(`/v1/creators/${encodeURIComponent(handle)}`),
    creatorListings: (handle: string, cursor?: string | null, limit = 24) => {
      const params = new URLSearchParams({ limit: String(limit) });
      if (cursor) params.set("cursor", cursor);
      return collection<Listing>(`/v1/creators/${encodeURIComponent(handle)}/listings?${params.toString()}`);
    },
    listing: (listingId: string) => request<Listing>(`/v1/listings/${listingId}`),
    mine: () => collection<Listing>("/v1/me/listings?limit=48"),
    create: (body: { assetId: string; title: string; description: string; tags: string[]; price: string; licenceOffer: { code: string; termsVersion: string } }) => request<Listing>("/v1/listings", { method: "POST", body: json(body) }, { csrf: true, idempotency: true }),
    update: (listingId: string, body: Partial<{ title: string; description: string; tags: string[]; price: string }>) => request<Listing>(`/v1/listings/${listingId}`, { method: "PATCH", body: json(body) }, { csrf: true, idempotency: true }),
    publish: (listingId: string) => request<Listing>(`/v1/listings/${listingId}/publish`, { method: "POST" }, { csrf: true, idempotency: true }),
    unpublish: (listingId: string) => request<Listing>(`/v1/listings/${listingId}/unpublish`, { method: "POST" }, { csrf: true, idempotency: true }),
    favorites: () => collection<{ assetId: string; createdAt: string; listing?: Listing | null }>("/v1/me/favorites?limit=48"),
    favorite: (assetId: string) => request<{ assetId: string }>(`/v1/assets/${assetId}/favorite`, { method: "POST" }, { csrf: true, idempotency: true }),
    unfavorite: (assetId: string) => request<void>(`/v1/assets/${assetId}/favorite`, { method: "DELETE" }, { csrf: true, idempotency: true }),
  },
  commerce: {
    checkout: (listing: Listing) => request<{ order: Order; paymentAttempt: { id: string; outTradeNo: string; status: string; expiresAt: string }; alipayForm: { action: string; method: "POST"; fields: Record<string, string> } }>("/v1/checkout", { method: "POST", body: json({ listingId: listing.id, licenceOfferId: listing.licenceOffer.id, channel: "desktop_web" }) }, { csrf: true, idempotency: true }),
    order: (orderId: string) => request<Order>(`/v1/orders/${orderId}`),
    // `fresh` skips the 30s collection cache — the payment-result screen polls
    // this endpoint waiting for the order to settle.
    purchases: (options: { fresh?: boolean } = {}) => collection<Order>("/v1/me/purchases?limit=48", options),
  },
  payouts: {
    list: () => collection<PayoutRequest>("/v1/me/payout-requests?limit=48"),
    balance: () => request<CreatorBalance>("/v1/me/payout-requests/balance"),
    create: (amount: string, qrCode: File) => {
      const body = new FormData();
      body.set("amount", amount);
      body.set("qrCode", qrCode);
      return request<PayoutRequest>("/v1/me/payout-requests", { method: "POST", body }, { csrf: true, idempotency: true });
    },
    cancel: (payoutId: string) => request<PayoutRequest>(`/v1/me/payout-requests/${payoutId}/cancel`, { method: "POST" }, { csrf: true, idempotency: true }),
  },
  finance: {
    payouts: (status?: PayoutRequest["status"]) => collection<InternalPayoutRequest>(`/internal/v1/payout-requests?limit=48${status ? `&status=${status}` : ""}`),
    markPaid: (payoutId: string, externalReference: string) => request<InternalPayoutRequest>(`/internal/v1/payout-requests/${payoutId}/mark-paid`, { method: "POST", body: json({ externalReference }) }, { csrf: true, idempotency: true }),
    reject: (payoutId: string, reason: string) => request<InternalPayoutRequest>(`/internal/v1/payout-requests/${payoutId}/reject`, { method: "POST", body: json({ reason }) }, { csrf: true, idempotency: true }),
  },
  admin: {
    statistics: () => request<AdminStatistics>("/internal/v1/admin/statistics"),
    computeNodes: () => request<AdminComputeNode[]>("/internal/v1/admin/compute-nodes"),
    users: (filters: { query?: string; status?: AdminUser["status"] | "all"; role?: "admin" | "creator" | "finance_operator" | "all" } = {}, cursor?: string | null) => {
      const params = new URLSearchParams({ limit: "100" });
      if (filters.query) params.set("q", filters.query);
      if (filters.status && filters.status !== "all") params.set("status", filters.status);
      if (filters.role && filters.role !== "all") params.set("role", filters.role);
      if (cursor) params.set("cursor", cursor);
      return collection<AdminUser>(`/internal/v1/admin/users?${params.toString()}`, { fresh: true });
    },
    updateUser: (userId: string, body: Partial<{ status: AdminUser["status"]; member: boolean; privilegedRoles: Array<"admin" | "finance_operator"> }>) => request<AdminUser>(`/internal/v1/admin/users/${userId}`, { method: "PATCH", body: json(body) }, { csrf: true, idempotency: true }),
    listings: (filters: { query?: string; status?: AdminListing["status"] | "all" } = {}, cursor?: string | null) => {
      const params = new URLSearchParams({ limit: "100" });
      if (filters.query) params.set("q", filters.query);
      if (filters.status && filters.status !== "all") params.set("status", filters.status);
      if (cursor) params.set("cursor", cursor);
      return collection<AdminListing>(`/internal/v1/admin/listings?${params.toString()}`, { fresh: true });
    },
    moderateListing: (listingId: string, action: "unpublish" | "archive", reason: string) => request<AdminListing>(`/internal/v1/admin/listings/${listingId}/moderate`, { method: "POST", body: json({ action, reason }) }, { csrf: true, idempotency: true }),
  },
  membership: {
    status: () => request<{ member: boolean }>("/v1/me/membership"),
    checkout: () => request<{ order: Order; paymentAttempt: { id: string; outTradeNo: string; status: string; expiresAt: string }; alipayForm: { action: string; method: "POST"; fields: Record<string, string> } }>("/v1/membership/checkout", { method: "POST" }, { csrf: true, idempotency: true }),
    grant: (email: string) => request<{ member: boolean }>("/internal/membership/grant", { method: "POST", body: json({ email }) }, { csrf: true, idempotency: true }),
  },
};

export function submitAlipayForm(form: { action: string; method: "POST"; fields: Record<string, string> }): void {
  const element = document.createElement("form");
  element.action = form.action;
  element.method = form.method;
  for (const [name, value] of Object.entries(form.fields)) {
    const input = document.createElement("input");
    input.type = "hidden";
    input.name = name;
    input.value = value;
    element.append(input);
  }
  document.body.append(element);
  element.submit();
}
