import { useQuery, useMutation, useQueryClient } from "@tanstack/react-query";
import { getApiClient } from "@/lib/api/client";

export const runKeys = {
  all: ["runs"] as const,
  lists: () => [...runKeys.all, "list"] as const,
  list: (params?: { limit?: number; offset?: number; module?: string; status?: string }) => [...runKeys.lists(), params] as const,
  details: () => [...runKeys.all, "detail"] as const,
  detail: (runId: string) => [...runKeys.details(), runId] as const,
};

const TERMINAL_STATUSES = new Set(["completed", "failed", "cancelled"]);
export function isRunTerminal(status: string): boolean { return TERMINAL_STATUSES.has(status); }

export function useRuns(params?: { limit?: number; offset?: number; module?: string; status?: string }) {
  return useQuery({ queryKey: runKeys.list(params), queryFn: () => getApiClient().runs.list(params) });
}
export function useRunStatus(runId: string | null) {
  return useQuery({ queryKey: runKeys.detail(runId!), queryFn: () => getApiClient().runs.status(runId!), enabled: !!runId,
    refetchInterval: (query) => { const status = query.state.data?.status; if (!status) return 3000; return isRunTerminal(status) ? false : 3000; },
  });
}
export function useActiveTasks() {
  return useQuery({ queryKey: ["tasks", "active"], queryFn: () => getApiClient().runs.activeTasks(), refetchInterval: 5000 });
}
export function useCancelRun() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (runId: string) => getApiClient().runs.cancel(runId),
    onSuccess: (_, runId) => { qc.invalidateQueries({ queryKey: runKeys.detail(runId) }); qc.invalidateQueries({ queryKey: runKeys.lists() }); },
  });
}
