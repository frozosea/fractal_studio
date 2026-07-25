import { ApiError, ApiErrorCode, mapHttpError } from "./errors";
import type { MapRenderRequest, MapRenderResponse, MapRenderInlineResponse, MapFieldRequest, MapFieldResponse, MapFieldSessionStartRequest, MapFieldSessionStatus, MapFieldSessionSnapshot, MapFieldSessionResult, LnMapRequest, LnMapResponse } from "@/types/map";
import type { SpecialPointEnumRequest, SpecialPointEnumResponse, SpecialPointSearchRequest, SpecialPointSearchResponse, SpecialPointSnapRequest, SpecialPointEnumResult } from "@/types/points";
import type { HsMeshRequest, HsFieldRequest, HsFieldResponse, MeshResponse, TransitionMeshRequest, TransitionVoxelRequest, TransitionVoxelResponse } from "@/types/mesh";
import type { VideoExportRequest, VideoExportResponse, VideoPreviewRequest, VideoPreviewResponse, VideoZoomRequest, VideoZoomResponse, TransitionVideoExportRequest, TransitionVideoExportResponse, TransitionVideoPreviewRequest, TransitionVideoPreviewResponse } from "@/types/video";
import type { RunRow, RunStatusResponse, ActiveTasksResponse } from "@/types/runs";
import type { VariantListResponse, VariantCompileResponse } from "@/types/variants";
import type { ArtifactRow } from "@/types/runs";
import type { Hardware } from "@/types/system";
import type { SpecialPoint } from "@/types/points";

// Config
export interface ApiClientConfig {
  baseUrl: string;
}

// Core request function
async function request<T>(config: ApiClientConfig, method: string, path: string, options?: { body?: unknown; params?: Record<string, string>; raw?: boolean; signal?: AbortSignal }): Promise<T> {
  const url = new URL(path, config.baseUrl);
  if (options?.params) {
    Object.entries(options.params).forEach(([k, v]) => { if (v !== undefined && v !== null) url.searchParams.set(k, v); });
  }
  let response: Response;
  try {
    response = await fetch(url.toString(), {
      method,
      headers: { "Content-Type": "application/json" },
      body: options?.body ? JSON.stringify(options.body) : undefined,
      signal: options?.signal,
    });
  } catch (err) {
    if (err instanceof DOMException && err.name === "AbortError") {
      throw err;
    }
    throw new ApiError(ApiErrorCode.NETWORK_ERROR, "Network error", { statusCode: 0 });
  }
  if (options?.raw) return response as unknown as T;
  if (response.status === 204) return undefined as T;
  let body: Record<string, unknown>;
  try { body = await response.json(); } catch { body = {}; }
  if (!response.ok) throw mapHttpError(response.status, body);
  return body as T;
}

// API methods organized by module
export interface ApiClient {
  system: {
    check: () => Promise<{ openmp: boolean; cuda: boolean }>;
    hardware: () => Promise<Hardware>;
    capabilities: () => Promise<Record<string, unknown>>;
  };
  map: {
    render: (req: MapRenderRequest, signal?: AbortSignal) => Promise<MapRenderResponse>;
    renderInline: (req: MapRenderRequest, signal?: AbortSignal) => Promise<MapRenderInlineResponse>;
    preempt: (req: { preemptKey: string; preemptSeq: number }) => Promise<{ status: string }>;
    field: (req: MapFieldRequest, signal?: AbortSignal) => Promise<MapFieldResponse>;
    lnMap: (req: LnMapRequest) => Promise<LnMapResponse>;
    // Interactive field sessions
    fieldSessionStart: (req: MapFieldSessionStartRequest, signal?: AbortSignal) => Promise<MapFieldSessionStatus>;
    fieldSessionStatus: (sessionId: string, signal?: AbortSignal) => Promise<MapFieldSessionStatus>;
    fieldSessionSnapshot: (sessionId: string, previewWidth: number, previewHeight: number, presentation?: { colorMap?: string; smooth?: boolean }, signal?: AbortSignal) => Promise<MapFieldSessionSnapshot>;
    fieldSessionResult: (sessionId: string, signal?: AbortSignal) => Promise<MapFieldSessionResult>;
    fieldSessionAcknowledge: (sessionId: string, requestId: string) => Promise<MapFieldSessionStatus>;
  };
  points: {
    auto: (k: number, p: number, pointType?: string) => Promise<{ mode: string; k: number; p: number; count: number; points: SpecialPoint[] }>;
    seed: (k: number, p: number, re: number, im: number) => Promise<{ mode: string; converged: boolean; points: SpecialPoint[] }>;
    list: (family?: string) => Promise<{ items: SpecialPoint[] }>;
    enumerate: (req: SpecialPointEnumRequest) => Promise<SpecialPointEnumResponse>;
    search: (req: SpecialPointSearchRequest, signal?: AbortSignal) => Promise<SpecialPointSearchResponse>;
    results: (runId: string) => Promise<SpecialPointSearchResponse>;
    snap: (req: SpecialPointSnapRequest) => Promise<{ point: SpecialPointEnumResult }>;
  };
  mesh: {
    hsMesh: (req: HsMeshRequest) => Promise<MeshResponse>;
    hsField: (req: HsFieldRequest) => Promise<HsFieldResponse>;
    transitionMesh: (req: TransitionMeshRequest) => Promise<MeshResponse>;
    transitionVoxels: (req: TransitionVoxelRequest) => Promise<TransitionVoxelResponse>;
  };
  video: {
    export: (req: VideoExportRequest) => Promise<VideoExportResponse>;
    preview: (req: VideoPreviewRequest) => Promise<VideoPreviewResponse>;
    zoom: (req: VideoZoomRequest) => Promise<VideoZoomResponse>;
    transitionExport: (req: TransitionVideoExportRequest) => Promise<TransitionVideoExportResponse>;
    transitionPreview: (req: TransitionVideoPreviewRequest) => Promise<TransitionVideoPreviewResponse>;
  };
  runs: {
    list: (params?: { limit?: number; offset?: number; module?: string; status?: string }) => Promise<{ items: RunRow[]; totalCount: number; modules: string[] }>;
    status: (runId: string) => Promise<RunStatusResponse>;
    activeTasks: () => Promise<ActiveTasksResponse>;
    cancel: (runId: string) => Promise<{ runId: string; status: string; accepted: boolean; cancelRequested: boolean }>;
  };
  artifacts: {
    list: (kind?: string, runId?: string) => Promise<{ items: ArtifactRow[] }>;
    contentUrl: (artifactId: string) => string;
    downloadUrl: (artifactId: string) => string;
  };
  variants: {
    list: () => Promise<VariantListResponse>;
    compile: (formula: string, name: string, bailout?: number) => Promise<VariantCompileResponse>;
    delete: (variantId: string) => Promise<{ ok: boolean }>;
  };
  benchmark: (req?: Record<string, unknown>) => Promise<Record<string, unknown>>;
}

// Factory
function createApiClient(config: ApiClientConfig): ApiClient {
  const req = <T>(method: string, path: string, options?: { body?: unknown; params?: Record<string, string>; signal?: AbortSignal }) =>
    request<T>(config, method, path, options);
  const base = config.baseUrl;

  return {
    system: {
      check: () => req("GET", "/api/system/check"),
      hardware: () => req("GET", "/api/system/hardware"),
      capabilities: () => req("GET", "/api/system/capabilities"),
    },
    map: {
      render: (body, signal) => request<MapRenderResponse>(config, "POST", "/api/map/render", { body, signal }),
      renderInline: async (body, signal) => {
        const url = new URL("/api/map/render-inline", base);
        const res = await fetch(url.toString(), {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify(body),
          signal,
        });
        if (res.status === 204) return { status: "cancelled", generatedMs: 0, width: body.width ?? 0, height: body.height ?? 0 };
        if (!res.ok) {
          const err = await res.json().catch(() => ({}));
          throw mapHttpError(res.status, err);
        }
        const data = await res.arrayBuffer();
        return {
          status: res.headers.get("X-FSD-Status") ?? "completed",
          requestId: res.headers.get("X-FSD-Request-Id") ?? undefined,
          data,
          generatedMs: Number(res.headers.get("X-FSD-Generated-Ms")) || 0,
          width: Number(res.headers.get("X-FSD-Width")) || body.width,
          height: Number(res.headers.get("X-FSD-Height")) || body.height,
          engineUsed: res.headers.get("X-FSD-Engine") ?? undefined,
          scalarUsed: res.headers.get("X-FSD-Scalar") ?? undefined,
          pixelFormat: res.headers.get("X-FSD-Pixel-Format") ?? undefined,
        };
      },
      preempt: (body) => req("POST", "/api/map/preempt", { body }),
      field: (body, signal) => request<MapFieldResponse>(config, "POST", "/api/map/field", { body, signal }),
      lnMap: (body) => req("POST", "/api/map/ln", { body }),
      fieldSessionStart: (body, signal) => req("POST", "/api/map/field/session/start", { body, signal }),
      fieldSessionStatus: (sessionId, signal) => req("POST", "/api/map/field/session/status", { body: { sessionId }, signal }),
      fieldSessionSnapshot: (sessionId, previewWidth, previewHeight, presentation, signal) =>
        req("POST", "/api/map/field/session/snapshot", { body: { sessionId, previewWidth, previewHeight, ...presentation }, signal }),
      fieldSessionResult: (sessionId, signal) => req("POST", "/api/map/field/session/result", { body: { sessionId }, signal }),
      fieldSessionAcknowledge: (sessionId, requestId) => req("POST", "/api/map/field/session/ack", { body: { sessionId, requestId } }),
    },
    points: {
      auto: (k, p, pointType) => req("POST", "/api/special-points/auto", { body: { k, p, pointType } }),
      seed: (k, p, re, im) => req("POST", "/api/special-points/seed", { body: { k, p, re, im } }),
      list: (family) => req("GET", `/api/special-points${family ? `?family=${encodeURIComponent(family)}` : ""}`),
      enumerate: (body) => req("POST", "/api/special-points/enumerate", { body }),
      search: (body, signal) => req("POST", "/api/special-points/search", { body, signal }),
      results: (runId) => req("GET", `/api/special-points/results?runId=${encodeURIComponent(runId)}`),
      snap: (body) => req("POST", "/api/special-points/snap", { body }),
    },
    mesh: {
      hsMesh: (body) => req("POST", "/api/hs/mesh", { body }),
      hsField: (body) => req("POST", "/api/hs/field", { body }),
      transitionMesh: (body) => req("POST", "/api/transition/mesh", { body }),
      transitionVoxels: (body) => req("POST", "/api/transition/voxels", { body }),
    },
    video: {
      export: (body) => req("POST", "/api/video/export", { body }),
      preview: (body) => req("POST", "/api/video/preview", { body }),
      zoom: (body) => req("POST", "/api/video/zoom", { body }),
      transitionExport: (body) => req("POST", "/api/video/transition", { body }),
      transitionPreview: (body) => req("POST", "/api/video/transition-preview", { body }),
    },
    runs: {
      list: (params) => {
        const q = new URLSearchParams();
        if (params?.limit !== undefined) q.set("limit", String(params.limit));
        if (params?.offset !== undefined) q.set("offset", String(params.offset));
        if (params?.module) q.set("module", params.module);
        if (params?.status) q.set("status", params.status);
        const s = q.toString();
        return req("GET", `/api/runs${s ? `?${s}` : ""}`);
      },
      status: (runId) => req("GET", `/api/runs/status?runId=${encodeURIComponent(runId)}`),
      activeTasks: () => req("GET", "/api/tasks/active"),
      cancel: (runId) => req("POST", `/api/runs/${encodeURIComponent(runId)}/cancel`, { body: {} }),
    },
    artifacts: {
      list: (kind, runId) => {
        const q = new URLSearchParams();
        if (kind) q.set("kind", kind);
        if (runId) q.set("runId", runId);
        const s = q.toString();
        return req("GET", `/api/artifacts${s ? `?${s}` : ""}`);
      },
      contentUrl: (artifactId) => `${base}/api/artifacts/content?artifactId=${encodeURIComponent(artifactId)}`,
      downloadUrl: (artifactId) => `${base}/api/artifacts/download?artifactId=${encodeURIComponent(artifactId)}`,
    },
    variants: {
      list: () => req("GET", "/api/variants"),
      compile: (formula, name, bailout) => req("POST", "/api/variants/compile", { body: bailout === undefined ? { formula, name } : { formula, name, bailout } }),
      delete: (variantId) => req("POST", "/api/variants/delete", { body: { variantId } }),
    },
    benchmark: (body = {}) => req("POST", "/api/benchmark", { body }),
  };
}

// Singleton
let _apiClient: ApiClient | null = null;

export function initApiClient(baseUrl?: string): ApiClient {
  _apiClient = createApiClient({ baseUrl: baseUrl ?? process.env.NEXT_PUBLIC_BACKEND_URL ?? "http://localhost:18080" });
  return _apiClient;
}

export function getApiClient(): ApiClient {
  if (!_apiClient) return initApiClient();
  return _apiClient;
}
