import { useMutation, useQueryClient } from "@tanstack/react-query";
import { getApiClient } from "@/lib/api/client";
import type { MapRenderRequest, MapFieldRequest } from "@/types/map";
import type { LnMapRequest } from "@/types/map";

export const mapKeys = {
  all: ["map"] as const,
  render: (req: MapRenderRequest) => ["map", "render", req] as const,
  field: (req: MapFieldRequest) => ["map", "field", req] as const,
  lnMap: (req: LnMapRequest) => ["map", "ln", req] as const,
};

export function useMapRender() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: ({ req, signal }: { req: MapRenderRequest; signal?: AbortSignal }) => getApiClient().map.render(req, signal),
    onSuccess: () => { qc.invalidateQueries({ queryKey: ["runs"] }); },
  });
}

export function useMapRenderInline() {
  return useMutation({
    mutationFn: ({ req, signal }: { req: MapRenderRequest; signal?: AbortSignal }) => getApiClient().map.renderInline(req, signal),
  });
}

export function useMapField() {
  return useMutation({
    mutationFn: ({ req, signal }: { req: MapFieldRequest; signal?: AbortSignal }) => getApiClient().map.field(req, signal),
  });
}

export function useLnMap() {
  return useMutation({
    mutationFn: (req: LnMapRequest) => getApiClient().map.lnMap(req),
  });
}

// Interactive field session hooks
export function useFieldSessionStart() {
  return useMutation({
    mutationFn: ({ req, signal }: { req: any; signal?: AbortSignal }) => getApiClient().map.fieldSessionStart(req, signal),
  });
}
export function useFieldSessionStatus() {
  return useMutation({
    mutationFn: ({ sessionId, signal }: { sessionId: string; signal?: AbortSignal }) => getApiClient().map.fieldSessionStatus(sessionId, signal),
  });
}
export function useFieldSessionSnapshot() {
  return useMutation({
    mutationFn: (params: { sessionId: string; previewWidth: number; previewHeight: number; presentation?: { colorMap?: string; smooth?: boolean }; signal?: AbortSignal }) =>
      getApiClient().map.fieldSessionSnapshot(params.sessionId, params.previewWidth, params.previewHeight, params.presentation, params.signal),
  });
}
export function useFieldSessionResult() {
  return useMutation({
    mutationFn: ({ sessionId, signal }: { sessionId: string; signal?: AbortSignal }) => getApiClient().map.fieldSessionResult(sessionId, signal),
  });
}
