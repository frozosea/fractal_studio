"use client";

/** Browser client for the public Platform API. Compute stays worker-only. */

export type Role = "creator" | "finance_operator" | string;

export interface PlatformUser {
  id: string;
  email: string;
  roles: Role[];
  status: "active" | "disabled";
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
  scale?: number;
  iterations?: number;
  variant?: string;
  colorMap?: string | null;
  metric?: "escape" | "min_abs" | "max_abs" | "envelope" | "min_pairwise_dist" | "mandel_ship_agree";
  smooth?: boolean;
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
  julia?: boolean;
  juliaRe?: number;
  juliaIm?: number;
  bailout?: number;
  engine?: "auto" | "cpu" | "cuda" | "openmp" | "avx2" | "avx512" | "hybrid";
  scalarType?: "auto" | "float" | "double" | "long_double" | "fp32" | "fp64" | "fx64" | "fp80" | "fp128";
}

export interface StudioCapabilities {
  rendererVersion?: string;
  metrics: string[];
  engines: string[];
  scalars: string[];
  colorMaps: string[];
  customGradient: { enabled: boolean; maxStops: number };
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
  derivativeStatus: "pending" | "ready" | "failed";
  derivativeErrorCode?: string | null;
  createdAt: string;
  files: Array<{ purpose: string; mediaType: string; sizeBytes: number }>;
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
  preview?: { thumbnailUrl?: string | null; watermarkedPreviewUrl?: string | null; videoPosterUrl?: string | null } | null;
  licenceOffer: { id: string; code: string; termsVersion: string; terms: Record<string, unknown> };
}

export interface Order {
  id: string;
  status: "pending_payment" | "fulfilled" | "closed" | "payment_exception";
  amount: string;
  currency: "CNY";
  paidAt?: string | null;
  createdAt: string;
  items: Array<{ assetId: string; listingId: string; price: string }>;
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

async function csrfToken(): Promise<string> {
  if (csrf) return csrf;
  const response = await fetch(`${baseUrl}/v1/auth/csrf-token`, { credentials: "include" });
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
  options: { csrf?: boolean; idempotency?: boolean; raw?: boolean } = {},
): Promise<T> {
  reportRequestActivity(1);
  try {
    const headers = new Headers(init.headers);
    headers.set("Accept", "application/json");
    const method = (init.method ?? "GET").toUpperCase();
    if (init.body && !(init.body instanceof FormData)) headers.set("Content-Type", "application/json");
    if (options.idempotency) headers.set("Idempotency-Key", idempotencyKey());
    if (options.csrf) headers.set("X-CSRF-Token", await csrfToken());
    const response = await fetch(`${baseUrl}${path}`, { ...init, method, headers, credentials: "include" });
    if (!response.ok) throw await asError(response);
    if (method !== "GET") collectionCache.clear();
    if (options.raw) return response as T;
    if (response.status === 204) return undefined as T;
    const body = (await response.json()) as { data: T };
    return body.data;
  } finally {
    reportRequestActivity(-1);
  }
}

function collection<T>(path: string): Promise<Page<T>> {
  const cached = collectionCache.get(path);
  if (cached && cached.expiresAt > Date.now()) return cached.promise as Promise<Page<T>>;

  reportRequestActivity(1);
  const pending = (async (): Promise<Page<T>> => {
    try {
      const response = await fetch(`${baseUrl}${path}`, { headers: { Accept: "application/json" }, credentials: "include" });
      if (!response.ok) throw await asError(response);
      return response.json() as Promise<Page<T>>;
    } finally {
      reportRequestActivity(-1);
    }
  })();
  const sharedPending = pending as Promise<Page<unknown>>;
  collectionCache.set(path, { expiresAt: Date.now() + COLLECTION_CACHE_TTL_MS, promise: sharedPending });
  void pending.catch(() => {
    if (collectionCache.get(path)?.promise === sharedPending) collectionCache.delete(path);
  });
  return pending;
}

function json(body: unknown): string {
  return JSON.stringify(body);
}

export const platform = {
  auth: {
    register: (email: string, password: string) => request<PlatformUser>("/v1/auth/register", { method: "POST", body: json({ email, password }) }),
    login: (email: string, password: string) => request<PlatformUser>("/v1/auth/login", { method: "POST", body: json({ email, password }) }),
    me: () => request<PlatformUser>("/v1/me"),
    logout: async () => {
      await request<void>("/v1/auth/logout", { method: "POST" }, { csrf: true });
      csrf = null;
    },
    creatorProfile: (handle: string, displayName: string) => request<PlatformUser>("/v1/me/creator-profile", { method: "PATCH", body: json({ handle, displayName }) }, { csrf: true, idempotency: true }),
  },
  studio: {
    preview: async (canonicalSpec: FractalSpec, width = 512, height = 512, signal?: AbortSignal): Promise<Blob> => {
      const response = await request<Response>("/v1/studio/preview", { method: "POST", body: json({ canonicalSpec, width, height }), signal }, { csrf: true, raw: true });
      return response.blob();
    },
    capabilities: () => request<StudioCapabilities>("/v1/studio/capabilities"),
    createRecipe: (canonicalSpec: FractalSpec) => request<Recipe>("/v1/recipes", { method: "POST", body: json({ canonicalSpec }) }, { csrf: true, idempotency: true }),
    recipes: () => collection<Recipe>("/v1/me/recipes"),
    createRender: (recipeId: string, output: Record<string, unknown>) => request<RenderJob>("/v1/render-jobs", { method: "POST", body: json({ recipeId, output }) }, { csrf: true, idempotency: true }),
    job: (jobId: string) => request<RenderJob>(`/v1/render-jobs/${jobId}`),
    cancel: (jobId: string) => request<RenderJob>(`/v1/render-jobs/${jobId}/cancel`, { method: "POST" }, { csrf: true, idempotency: true }),
  },
  assets: {
    list: () => collection<Asset>("/v1/me/assets?limit=48"),
    get: (assetId: string) => request<Asset>(`/v1/me/assets/${assetId}`),
    setVisibility: (assetId: string, visibility: "private" | "hidden") => request<Asset>(`/v1/me/assets/${assetId}`, { method: "PATCH", body: json({ visibility }) }, { csrf: true, idempotency: true }),
    remove: (assetId: string) => request<void>(`/v1/me/assets/${assetId}`, { method: "DELETE" }, { csrf: true, idempotency: true }),
    downloadUrl: (assetId: string) => request<{ url: string; expiresAt: string }>(`/v1/assets/${assetId}/download-url`, { method: "POST" }, { csrf: true }),
  },
  marketplace: {
    explore: (query = "") => collection<Listing>(`/v1/explore?limit=24${query ? `&q=${encodeURIComponent(query)}` : ""}`),
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
    purchases: () => collection<Order>("/v1/me/purchases?limit=48"),
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
