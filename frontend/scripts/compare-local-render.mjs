import { spawnSync } from "node:child_process";
import { readFile } from "node:fs/promises";
import { resolve } from "node:path";
import ts from "typescript";

const reference = process.argv.at(-1) === "--" || process.argv.length < 3
  ? "../backend/build/browser_orbit_reference" : process.argv.at(-1);
const source = await readFile(resolve("src/lib/fractal/local-render-core.ts"), "utf8");
const compiled = ts.transpileModule(source, { compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2022 } });
const core = await import(`data:text/javascript;base64,${Buffer.from(compiled.outputText).toString("base64")}`);
const nativeRun = spawnSync(reference, { encoding: "utf8" });
if (!nativeRun.stdout) throw nativeRun.error ?? new Error(nativeRun.stderr || "native reference failed");
const lines = nativeRun.stdout.trim().split("\n");
let compared = 0;
const failures = [];
for (const line of lines) {
  const [variant, re, im, julia, jr, ji, maxIter, expectedIter, expectedNorm, expectedMin, expectedMax] = line.split("\t");
  const base = {
    centerRe: 0, centerIm: 0, scale: 3, iterations: Number(maxIter), variant,
    metric: "escape", colorMap: "classic_cos", colorMode: "direct", cyclesPerOctave: 1,
    smooth: false, rotationDeg: 0, julia: julia === "1", juliaRe: Number(jr), juliaIm: Number(ji),
    bailout: ["sin_z","cos_z","exp_z","sinh_z","cosh_z","tan_z"].includes(variant) ? 64 : 2,
  };
  const escape = core.iterateOrbit(base, Number(re), Number(im));
  const minimum = core.iterateOrbit({ ...base, metric: "min_abs" }, Number(re), Number(im));
  const maximum = core.iterateOrbit({ ...base, metric: "max_abs" }, Number(re), Number(im));
  const close = (actual, expected) => !Number.isFinite(Number(expected)) || Math.abs(actual - Number(expected)) <= 1e-11 * Math.max(1, Math.abs(Number(expected)));
  if (escape.iter !== Number(expectedIter) || !close(escape.norm, expectedNorm) || !close(minimum.field, expectedMin) || !close(maximum.field, expectedMax)) {
    failures.push(`${variant}@${re},${im}: browser=${escape.iter}/${escape.norm} native=${expectedIter}/${expectedNorm}`);
  }
  compared += 1;
}
if (failures.length) {
  console.error(failures.join("\n"));
  process.exit(1);
}
console.log(`browser/native orbit parity: ${compared} samples passed`);

const frameCases = [
  { name: "mandelbrot", variant: "mandelbrot", julia: false, colorMap: "classic_cos", smooth: false, bailout: 2 },
  { name: "burning_ship_julia", variant: "burning_ship", julia: true, colorMap: "viridis", smooth: true, bailout: 2 },
];
for (const frame of frameCases) {
  const run = spawnSync(reference, ["--frame", frame.name]);
  if (!run.stdout?.length) throw run.error ?? new Error(`native frame ${frame.name} failed`);
  const actual = core.renderLocalRgba({ centerRe: -.64, centerIm: .03, scale: 2.4, iterations: 180,
    metric: "escape", colorMode: "direct", cyclesPerOctave: 1, rotationDeg: 0,
    juliaRe: -.8, juliaIm: .156, ...frame }, 32, 24);
  let maxDelta = 0; let changed = 0;
  for (let index = 0; index < actual.length; index += 1) {
    const delta = Math.abs(actual[index] - run.stdout[index]); maxDelta = Math.max(maxDelta, delta); if (delta > 1) changed += 1;
  }
  if (maxDelta > 1 || changed) throw new Error(`${frame.name} RGBA mismatch: max=${maxDelta}, changed=${changed}`);
  console.log(`browser/native RGBA parity: ${frame.name} passed (${actual.length / 4} pixels)`);
}
