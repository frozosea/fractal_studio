"use client";

import { useState } from "react";
import { useTranslations } from "next-intl";
import { Tabs, TabsContent, TabsList, TabsTrigger } from "@/components/ui/tabs";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { Card, CardContent, CardHeader, CardTitle } from "@/components/ui/card";
import { useHsMesh, useTransitionMesh, useTransitionVoxels } from "@/lib/hooks/use-mesh";
import type { HsMeshRequest, TransitionMeshRequest, TransitionVoxelRequest } from "@/types/mesh";

export default function MeshPage() {
  const t = useTranslations("mesh");
  const hsMeshMutation = useHsMesh();
  const transMeshMutation = useTransitionMesh();
  const voxelMutation = useTransitionVoxels();

  const [hsResult, setHsResult] = useState<any>(null);
  const [transResult, setTransResult] = useState<any>(null);
  const [voxelResult, setVoxelResult] = useState<any>(null);

  return (
    <div className="space-y-6">
      <h1 className="text-2xl font-bold tracking-tight">{t("title")}</h1>

      <Tabs defaultValue="hs">
        <TabsList>
          <TabsTrigger value="hs">{t("heightfield")}</TabsTrigger>
          <TabsTrigger value="transition">{t("transition3d")}</TabsTrigger>
        </TabsList>

        <TabsContent value="hs" className="mt-4 space-y-4">
          <Card>
            <CardHeader>
              <CardTitle>{t("hsMeshTitle")}</CardTitle>
            </CardHeader>
            <CardContent className="space-y-4">
              <div className="grid grid-cols-3 gap-4">
                <div>
                  <label className="text-sm text-muted-foreground">Center Re</label>
                  <Input type="number" defaultValue={-0.75} id="hs-re" />
                </div>
                <div>
                  <label className="text-sm text-muted-foreground">Center Im</label>
                  <Input type="number" defaultValue={0} id="hs-im" />
                </div>
                <div>
                  <label className="text-sm text-muted-foreground">Scale</label>
                  <Input type="number" defaultValue={3.0} id="hs-scale" step={0.1} />
                </div>
                <div>
                  <label className="text-sm text-muted-foreground">Resolution</label>
                  <Input type="number" defaultValue={256} id="hs-res" />
                </div>
                <div>
                  <label className="text-sm text-muted-foreground">Iterations</label>
                  <Input type="number" defaultValue={256} id="hs-iters" />
                </div>
                <div>
                  <label className="text-sm text-muted-foreground">Height Scale</label>
                  <Input type="number" defaultValue={1.0} id="hs-hscale" step={0.1} />
                </div>
              </div>
              <Button
                variant="fractal"
                onClick={async () => {
                  const req: HsMeshRequest = {
                    centerRe: Number((document.getElementById("hs-re") as HTMLInputElement)?.value) || -0.75,
                    centerIm: Number((document.getElementById("hs-im") as HTMLInputElement)?.value) || 0,
                    scale: Number((document.getElementById("hs-scale") as HTMLInputElement)?.value) || 3.0,
                    resolution: Number((document.getElementById("hs-res") as HTMLInputElement)?.value) || 256,
                    iterations: Number((document.getElementById("hs-iters") as HTMLInputElement)?.value) || 256,
                    heightScale: Number((document.getElementById("hs-hscale") as HTMLInputElement)?.value) || 1.0,
                    metric: "min_abs",
                  };
                  const result = await hsMeshMutation.mutateAsync(req);
                  setHsResult(result);
                }}
                disabled={hsMeshMutation.isPending}
              >
                {hsMeshMutation.isPending ? t("generating") : t("generateMesh")}
              </Button>
              {hsResult && (
                <div className="rounded-lg border border-white/10 p-4">
                  <p>Vertices: {hsResult.vertexCount}</p>
                  <p>Triangles: {hsResult.triangleCount}</p>
                  <p>Generated: {hsResult.generatedMs?.toFixed(0)}ms</p>
                  {hsResult.glbUrl && (
                    <a href={hsResult.glbUrl} className="text-fractal-400 hover:underline" target="_blank" rel="noreferrer">
                      Download GLB
                    </a>
                  )}
                </div>
              )}
            </CardContent>
          </Card>
        </TabsContent>

        <TabsContent value="transition" className="mt-4 space-y-4">
          <Card>
            <CardHeader>
              <CardTitle>{t("transitionMeshTitle")}</CardTitle>
            </CardHeader>
            <CardContent className="space-y-4">
              <div className="grid grid-cols-3 gap-4">
                <div>
                  <label className="text-sm text-muted-foreground">Extent</label>
                  <Input type="number" defaultValue={3.0} id="tm-extent" step={0.1} />
                </div>
                <div>
                  <label className="text-sm text-muted-foreground">Resolution</label>
                  <Input type="number" defaultValue={128} id="tm-res" />
                </div>
                <div>
                  <label className="text-sm text-muted-foreground">Iterations</label>
                  <Input type="number" defaultValue={128} id="tm-iters" />
                </div>
                <div>
                  <label className="text-sm text-muted-foreground">ISO Value</label>
                  <Input type="number" defaultValue={0.5} id="tm-iso" step={0.1} />
                </div>
                <div>
                  <label className="text-sm text-muted-foreground">Transition From</label>
                  <Input type="text" defaultValue="mandelbrot" id="tm-from" />
                </div>
                <div>
                  <label className="text-sm text-muted-foreground">Transition To</label>
                  <Input type="text" defaultValue="burning_ship" id="tm-to" />
                </div>
              </div>
              <div className="flex gap-4">
                <Button
                  variant="fractal"
                  onClick={async () => {
                    const req: TransitionMeshRequest = {
                      extent: Number((document.getElementById("tm-extent") as HTMLInputElement)?.value) || 3.0,
                      resolution: Number((document.getElementById("tm-res") as HTMLInputElement)?.value) || 128,
                      iterations: Number((document.getElementById("tm-iters") as HTMLInputElement)?.value) || 128,
                      iso: Number((document.getElementById("tm-iso") as HTMLInputElement)?.value) || 0.5,
                      transitionFrom: (document.getElementById("tm-from") as HTMLInputElement)?.value || "mandelbrot",
                      transitionTo: (document.getElementById("tm-to") as HTMLInputElement)?.value || "burning_ship",
                    };
                    const result = await transMeshMutation.mutateAsync(req);
                    setTransResult(result);
                  }}
                  disabled={transMeshMutation.isPending}
                >
                  {transMeshMutation.isPending ? t("generating") : t("generateMesh")}
                </Button>
                <Button
                  variant="neon"
                  onClick={async () => {
                    const req: TransitionVoxelRequest = {
                      extent: Number((document.getElementById("tm-extent") as HTMLInputElement)?.value) || 3.0,
                      resolution: Number((document.getElementById("tm-res") as HTMLInputElement)?.value) || 64,
                      iterations: Number((document.getElementById("tm-iters") as HTMLInputElement)?.value) || 128,
                      transitionFrom: (document.getElementById("tm-from") as HTMLInputElement)?.value || "mandelbrot",
                      transitionTo: (document.getElementById("tm-to") as HTMLInputElement)?.value || "burning_ship",
                    };
                    const result = await voxelMutation.mutateAsync(req);
                    setVoxelResult(result);
                  }}
                  disabled={voxelMutation.isPending}
                >
                  {voxelMutation.isPending ? t("generating") : t("generateVoxels")}
                </Button>
              </div>
              {(transResult || voxelResult) && (
                <div className="rounded-lg border border-white/10 p-4 space-y-2">
                  {transResult && <p>Mesh — Triangles: {transResult.triangleCount}</p>}
                  {voxelResult && <p>Voxels — Faces: {voxelResult.faceCount}, Resolution: {voxelResult.resolution}</p>}
                </div>
              )}
            </CardContent>
          </Card>
        </TabsContent>
      </Tabs>
    </div>
  );
}
