import { z } from "zod";

export const mapRenderSchema = z.object({
  centerRe: z.number().finite(),
  centerIm: z.number().finite(),
  scale: z.number().positive().finite(),
  width: z.number().int().min(64).max(16384),
  height: z.number().int().min(64).max(16384),
  iterations: z.number().int().min(1).max(1000000),
  variant: z.string().min(1),
  metric: z.enum(["escape", "min_abs", "max_abs", "envelope", "min_pairwise_dist", "mandel_ship_agree"]),
  colorMap: z.string(),
  smooth: z.boolean().optional(),
  julia: z.boolean().optional(),
  juliaRe: z.number().optional(),
  juliaIm: z.number().optional(),
  engine: z.string().optional(),
  scalarType: z.string().optional(),
  rotationDeg: z.number().optional(),
});

export const variantCompileSchema = z.object({
  name: z.string().min(1).max(80),
  formula: z.string().min(1).max(500),
  bailout: z.number().positive().optional(),
});
