import { useQuery, useMutation, useQueryClient } from "@tanstack/react-query";
import { getApiClient } from "@/lib/api/client";

export function useVariants() {
  return useQuery({ queryKey: ["variants"], queryFn: () => getApiClient().variants.list() });
}
export function useCompileVariant() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: ({ formula, name, bailout }: { formula: string; name: string; bailout?: number }) => getApiClient().variants.compile(formula, name, bailout),
    onSuccess: () => qc.invalidateQueries({ queryKey: ["variants"] }),
  });
}
export function useDeleteVariant() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (variantId: string) => getApiClient().variants.delete(variantId),
    onSuccess: () => qc.invalidateQueries({ queryKey: ["variants"] }),
  });
}
