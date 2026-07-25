import { useQuery, useMutation, useQueryClient } from "@tanstack/react-query";
import { getApiClient } from "@/lib/api/client";
import type { SpecialPointEnumRequest, SpecialPointSearchRequest, SpecialPointSnapRequest } from "@/types/points";

export const pointKeys = {
  all: ["points"] as const,
  list: (family?: string) => ["points", "list", family] as const,
  searchResults: (runId: string) => ["points", "results", runId] as const,
};

export function useSpecialPointsList(family?: string) {
  return useQuery({ queryKey: pointKeys.list(family), queryFn: () => getApiClient().points.list(family) });
}
export function useSpecialPointsEnumerate() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (req: SpecialPointEnumRequest) => getApiClient().points.enumerate(req),
    onSuccess: () => qc.invalidateQueries({ queryKey: pointKeys.all }),
  });
}
export function useSpecialPointsSearch() {
  return useMutation({
    mutationFn: ({ req, signal }: { req: SpecialPointSearchRequest; signal?: AbortSignal }) => getApiClient().points.search(req, signal),
  });
}
export function useSpecialPointsResults(runId: string) {
  return useQuery({ queryKey: pointKeys.searchResults(runId), queryFn: () => getApiClient().points.results(runId), enabled: !!runId });
}
export function useSpecialPointsSnap() {
  return useMutation({
    mutationFn: (req: SpecialPointSnapRequest) => getApiClient().points.snap(req),
  });
}
