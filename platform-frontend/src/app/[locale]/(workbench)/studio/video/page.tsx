"use client";

import { useState } from "react";
import { useTranslations } from "next-intl";
import { Tabs, TabsContent, TabsList, TabsTrigger } from "@/components/ui/tabs";
import { Card, CardContent, CardHeader, CardTitle } from "@/components/ui/card";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { useStudioStore } from "@/stores/studio-store";
import { useVideoExport, useVideoPreview, useTransitionVideoExport, useTransitionVideoPreview } from "@/lib/hooks/use-video";
import type { VideoExportRequest, TransitionVideoExportRequest } from "@/types/video";

export default function VideoPage() {
  const t = useTranslations("video");
  const store = useStudioStore();
  const exportMutation = useVideoExport();
  const previewMutation = useVideoPreview();
  const transExportMutation = useTransitionVideoExport();
  const transPreviewMutation = useTransitionVideoPreview();
  const [result, setResult] = useState<any>(null);
  const [previewData, setPreviewData] = useState<any>(null);

  const buildVideoReq = (): VideoExportRequest => ({
    centerRe: store.centerRe,
    centerIm: store.centerIm,
    variant: store.variant,
    colorMap: store.colorMap as VideoExportRequest["colorMap"],
    iterations: store.iterations,
    width: 1920,
    height: 1080,
    depthOctaves: 16,
    fps: 30,
    secondsPerOctave: 4,
    qualityPreset: "balanced",
    background: true,
  });

  return (
    <div className="space-y-6">
      <h1 className="text-2xl font-bold tracking-tight">{t("title")}</h1>

      <Tabs defaultValue="zoom">
        <TabsList>
          <TabsTrigger value="zoom">{t("zoomVideo")}</TabsTrigger>
          <TabsTrigger value="transition">{t("transitionVideo")}</TabsTrigger>
        </TabsList>

        <TabsContent value="zoom" className="mt-4 space-y-4">
          <Card>
            <CardHeader><CardTitle>{t("zoomExportTitle")}</CardTitle></CardHeader>
            <CardContent className="space-y-4">
              <div className="grid grid-cols-3 gap-4">
                <div>
                  <label className="text-sm text-muted-foreground">FPS</label>
                  <Input type="number" defaultValue={30} id="v-fps" />
                </div>
                <div>
                  <label className="text-sm text-muted-foreground">Depth Octaves</label>
                  <Input type="number" defaultValue={16} id="v-depth" />
                </div>
                <div>
                  <label className="text-sm text-muted-foreground">Seconds/Octave</label>
                  <Input type="number" defaultValue={4} id="v-spo" step={0.5} />
                </div>
                <div>
                  <label className="text-sm text-muted-foreground">Width</label>
                  <Input type="number" defaultValue={1920} id="v-width" />
                </div>
                <div>
                  <label className="text-sm text-muted-foreground">Height</label>
                  <Input type="number" defaultValue={1080} id="v-height" />
                </div>
                <div>
                  <label className="text-sm text-muted-foreground">Quality</label>
                  <Input type="text" defaultValue="balanced" id="v-quality" />
                </div>
              </div>
              <div className="flex gap-4">
                <Button variant="fractal" onClick={async () => {
                  const req = buildVideoReq();
                  const r = await previewMutation.mutateAsync(req);
                  setPreviewData(r);
                }} disabled={previewMutation.isPending}>
                  {previewMutation.isPending ? t("generating") : t("preview")}
                </Button>
                <Button variant="neon" onClick={async () => {
                  const req = buildVideoReq();
                  const r = await exportMutation.mutateAsync(req);
                  setResult(r);
                }} disabled={exportMutation.isPending}>
                  {exportMutation.isPending ? t("exporting") : t("exportFull")}
                </Button>
              </div>
              {previewData && (
                <div className="rounded-lg border border-white/10 p-4">
                  <p>Preview ready — {previewData.frameCount} frames @ {previewData.fps}fps</p>
                  {previewData.startFrameUrl && (
                    <img src={previewData.startFrameUrl} alt="Start frame" className="mt-2 max-h-48 rounded" />
                  )}
                </div>
              )}
              {result && (
                <div className="rounded-lg border border-white/10 p-4">
                  <p>Export: Run {result.runId} — Status: {result.status}</p>
                  <p>Frames: {result.frameCount} @ {result.fps}fps, Duration: {result.durationSec}s</p>
                  {result.videoDownloadUrl && (
                    <a href={result.videoDownloadUrl} className="text-fractal-400 hover:underline" target="_blank" rel="noreferrer">
                      Download Video
                    </a>
                  )}
                </div>
              )}
            </CardContent>
          </Card>
        </TabsContent>

        <TabsContent value="transition" className="mt-4 space-y-4">
          <Card>
            <CardHeader><CardTitle>{t("transitionExportTitle")}</CardTitle></CardHeader>
            <CardContent className="space-y-4">
              <div className="grid grid-cols-3 gap-4">
                <div>
                  <label className="text-sm text-muted-foreground">From Variant</label>
                  <Input type="text" defaultValue="mandelbrot" id="tv-from" />
                </div>
                <div>
                  <label className="text-sm text-muted-foreground">To Variant</label>
                  <Input type="text" defaultValue="burning_ship" id="tv-to" />
                </div>
                <div>
                  <label className="text-sm text-muted-foreground">Duration (sec)</label>
                  <Input type="number" defaultValue={10} id="tv-dur" />
                </div>
                <div>
                  <label className="text-sm text-muted-foreground">FPS</label>
                  <Input type="number" defaultValue={30} id="tv-fps" />
                </div>
                <div>
                  <label className="text-sm text-muted-foreground">Width</label>
                  <Input type="number" defaultValue={1920} id="tv-width" />
                </div>
                <div>
                  <label className="text-sm text-muted-foreground">Height</label>
                  <Input type="number" defaultValue={1080} id="tv-height" />
                </div>
              </div>
              <div className="flex gap-4">
                <Button variant="fractal" onClick={async () => {
                  const req: TransitionVideoExportRequest = {
                    centerRe: store.centerRe,
                    centerIm: store.centerIm,
                    scale: store.scale,
                    transitionFrom: (document.getElementById("tv-from") as HTMLInputElement)?.value || "mandelbrot",
                    transitionTo: (document.getElementById("tv-to") as HTMLInputElement)?.value || "burning_ship",
                    durationSec: Number((document.getElementById("tv-dur") as HTMLInputElement)?.value) || 10,
                    fps: Number((document.getElementById("tv-fps") as HTMLInputElement)?.value) || 30,
                    width: Number((document.getElementById("tv-width") as HTMLInputElement)?.value) || 1920,
                    height: Number((document.getElementById("tv-height") as HTMLInputElement)?.value) || 1080,
                    iterations: store.iterations,
                    colorMap: store.colorMap as VideoExportRequest["colorMap"],
                    background: true,
                  };
                  const r = await transPreviewMutation.mutateAsync(req);
                  setPreviewData(r);
                }} disabled={transPreviewMutation.isPending}>
                  {transPreviewMutation.isPending ? t("generating") : t("preview")}
                </Button>
                <Button variant="neon" onClick={async () => {
                  const req: TransitionVideoExportRequest = {
                    centerRe: store.centerRe,
                    centerIm: store.centerIm,
                    scale: store.scale,
                    transitionFrom: (document.getElementById("tv-from") as HTMLInputElement)?.value || "mandelbrot",
                    transitionTo: (document.getElementById("tv-to") as HTMLInputElement)?.value || "burning_ship",
                    durationSec: Number((document.getElementById("tv-dur") as HTMLInputElement)?.value) || 10,
                    fps: Number((document.getElementById("tv-fps") as HTMLInputElement)?.value) || 30,
                    width: Number((document.getElementById("tv-width") as HTMLInputElement)?.value) || 1920,
                    height: Number((document.getElementById("tv-height") as HTMLInputElement)?.value) || 1080,
                    iterations: store.iterations,
                    colorMap: store.colorMap as VideoExportRequest["colorMap"],
                    background: true,
                  };
                  const r = await transExportMutation.mutateAsync(req);
                  setResult(r);
                }} disabled={transExportMutation.isPending}>
                  {transExportMutation.isPending ? t("exporting") : t("exportFull")}
                </Button>
              </div>
              {(previewData || result) && (
                <div className="rounded-lg border border-white/10 p-4">
                  {previewData && <p>Preview — {previewData.frameCount} frames</p>}
                  {result && (
                    <>
                      <p>Run: {result.runId} — Status: {result.status}</p>
                      {result.videoDownloadUrl && (
                        <a href={result.videoDownloadUrl} className="text-fractal-400 hover:underline" target="_blank" rel="noreferrer">
                          Download Video
                        </a>
                      )}
                    </>
                  )}
                </div>
              )}
            </CardContent>
          </Card>
        </TabsContent>
      </Tabs>
    </div>
  );
}
