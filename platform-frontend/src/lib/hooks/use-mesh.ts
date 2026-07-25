import { useMutation } from "@tanstack/react-query";
import { getApiClient } from "@/lib/api/client";
import type { HsMeshRequest, HsFieldRequest, TransitionMeshRequest, TransitionVoxelRequest } from "@/types/mesh";

export function useHsMesh() {
  return useMutation({
    mutationFn: (req: HsMeshRequest) => getApiClient().mesh.hsMesh(req),
  });
}

export function useHsField() {
  return useMutation({
    mutationFn: (req: HsFieldRequest) => getApiClient().mesh.hsField(req),
  });
}

export function useTransitionMesh() {
  return useMutation({
    mutationFn: (req: TransitionMeshRequest) => getApiClient().mesh.transitionMesh(req),
  });
}

export function useTransitionVoxels() {
  return useMutation({
    mutationFn: (req: TransitionVoxelRequest) => getApiClient().mesh.transitionVoxels(req),
  });
}
