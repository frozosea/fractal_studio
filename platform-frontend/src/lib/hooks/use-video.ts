import { useMutation } from "@tanstack/react-query";
import { getApiClient } from "@/lib/api/client";
import type { VideoExportRequest, VideoPreviewRequest, VideoZoomRequest, TransitionVideoExportRequest, TransitionVideoPreviewRequest } from "@/types/video";

export function useVideoExport() {
  return useMutation({
    mutationFn: (req: VideoExportRequest) => getApiClient().video.export(req),
  });
}

export function useVideoPreview() {
  return useMutation({
    mutationFn: (req: VideoPreviewRequest) => getApiClient().video.preview(req),
  });
}

export function useVideoZoom() {
  return useMutation({
    mutationFn: (req: VideoZoomRequest) => getApiClient().video.zoom(req),
  });
}

export function useTransitionVideoExport() {
  return useMutation({
    mutationFn: (req: TransitionVideoExportRequest) => getApiClient().video.transitionExport(req),
  });
}

export function useTransitionVideoPreview() {
  return useMutation({
    mutationFn: (req: TransitionVideoPreviewRequest) => getApiClient().video.transitionPreview(req),
  });
}
