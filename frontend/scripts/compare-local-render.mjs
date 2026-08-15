import { spawnSync } from "node:child_process";
import { readFile } from "node:fs/promises";
import { resolve } from "node:path";
import ts from "typescript";

const reference = process.argv.at(-1) === "--" || process.argv.length < 3
  ? "../backend/build/browser_orbit_reference" : process.argv.at(-1);
const orbitSource = await readFile(resolve("src/lib/fractal/local-orbit-program.ts"), "utf8");
const orbitCompiled = ts.transpileModule(orbitSource, { compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2022 } });
const orbitUrl = `data:text/javascript;base64,${Buffer.from(orbitCompiled.outputText).toString("base64")}`;
const orbit = await import(orbitUrl);
const cacheSource = await readFile(resolve("src/lib/fractal/local-field-cache.ts"), "utf8");
const cacheCompiled = ts.transpileModule(cacheSource, { compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2022 } });
const cacheUrl = `data:text/javascript;base64,${Buffer.from(cacheCompiled.outputText).toString("base64")}`;
const cache = await import(cacheUrl);
const source = await readFile(resolve("src/lib/fractal/local-render-core.ts"), "utf8");
const compiled = ts.transpileModule(source, { compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2022 } });
const coreCode = compiled.outputText
  .replace('"./local-orbit-program"', JSON.stringify(orbitUrl))
  .replace('"./local-field-cache"', JSON.stringify(cacheUrl));
const core = await import(`data:text/javascript;base64,${Buffer.from(coreCode).toString("base64")}`);
const transitionSource = await readFile(resolve("src/lib/fractal/local-transition-core.ts"), "utf8");
const transitionCompiled = ts.transpileModule(transitionSource, { compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2022 } });
const transition = await import(`data:text/javascript;base64,${Buffer.from(transitionCompiled.outputText).toString("base64")}`);
const nativeRun = spawnSync(reference, { encoding: "utf8" });
if (!nativeRun.stdout) throw nativeRun.error ?? new Error(nativeRun.stderr || "native reference failed");
const lines = nativeRun.stdout.trim().split("\n");
let compared = 0;
const failures = [];
for (const line of lines) {
  const [variant, re, im, julia, jr, ji, maxIter, expectedIter, expectedNorm, expectedMin, expectedMax, expectedPairwise] = line.split("\t");
  const base = {
    centerRe: 0, centerIm: 0, scale: 3, iterations: Number(maxIter), variant,
    metric: "escape", colorMap: "classic_cos", colorMode: "direct", cyclesPerOctave: 1,
    smooth: false, rotationDeg: 0, julia: julia === "1", juliaRe: Number(jr), juliaIm: Number(ji),
    bailout: ["sin_z","cos_z","exp_z","sinh_z","cosh_z","tan_z"].includes(variant) ? 64 : 2,
  };
  const escape = core.iterateOrbit(base, Number(re), Number(im));
  const minimum = core.iterateOrbit({ ...base, metric: "min_abs" }, Number(re), Number(im));
  const maximum = core.iterateOrbit({ ...base, metric: "max_abs" }, Number(re), Number(im));
  const pairwise = core.iterateOrbit({ ...base, metric: "min_pairwise_dist", pairwiseCap:64 }, Number(re), Number(im));
  const close = (actual, expected) => !Number.isFinite(Number(expected)) || Math.abs(actual - Number(expected)) <= 1e-11 * Math.max(1, Math.abs(Number(expected)));
  if (escape.iter !== Number(expectedIter) || !close(escape.norm, expectedNorm) || !close(minimum.field, expectedMin) || !close(maximum.field, expectedMax) || !close(pairwise.field, expectedPairwise)) {
    failures.push(`${variant}@${re},${im}: browser=${escape.iter}/${escape.norm} native=${expectedIter}/${expectedNorm}`);
  }
  compared += 1;
}
if (failures.length) {
  console.error(failures.join("\n"));
  process.exit(1);
}
console.log(`browser/native orbit + pairwise parity: ${compared} samples passed`);

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

const agreementFrameRun=spawnSync(reference,["--frame","burning_ship_agree"]);
if(!agreementFrameRun.stdout?.length)throw agreementFrameRun.error??new Error("native agreement frame failed");
const agreementFrameSpec={centerRe:-.64,centerIm:.03,scale:2.4,iterations:120,variant:"burning_ship",metric:"mandel_ship_agree",
  colorMap:"twilight",colorMode:"direct",cyclesPerOctave:1,smooth:false,rotationDeg:19,julia:false,juliaRe:-.8,juliaIm:.156,bailout:2};
const agreementFrameRaw=transition.renderLocalMandelShipAgreementRaw(agreementFrameSpec,32,24);
const agreementFrame=core.colorizeLocalAgreement(agreementFrameSpec,agreementFrameRaw.agreementIterU32,agreementFrameRaw.fieldF64,32,24);
let agreementFrameDelta=0,agreementChangedPixels=0;
for(let pixel=0;pixel<agreementFrame.length/4;++pixel){let changed=false;for(let channel=0;channel<4;++channel){const index=pixel*4+channel;const delta=Math.abs(agreementFrame[index]-agreementFrameRun.stdout[index]);agreementFrameDelta=Math.max(agreementFrameDelta,delta);changed ||= delta>1;}if(changed)agreementChangedPixels+=1;}
if(agreementChangedPixels>8)throw new Error(`agreement RGBA mismatch: max=${agreementFrameDelta}, changed pixels=${agreementChangedPixels}`);
console.log(`browser/native RGBA parity: burning_ship_agree passed (768 pixels, ${agreementChangedPixels} floating-boundary pixels)`);

const programRun = spawnSync(reference, ["--program"], { encoding: "utf8" });
if (!programRun.stdout) throw programRun.error ?? new Error(programRun.stderr || "native program reference failed");
const programs = {
  polynomial: orbit.compileLocalOrbitProgram({ type:"formula", formula:{ type:"dsl", source:"z^2 + alpha*c + i*beta", parameters:{ alpha:0.75, beta:0.1 } } }),
  analytic: orbit.compileLocalOrbitProgram({ type:"formula", formula:{ type:"dsl", source:"sin(z)+conj(c)/3" } }),
  sequence: orbit.compileLocalOrbitProgram({ type:"sequence", repeat:true, steps:[
    { span:2, program:{ type:"formula", formula:{ type:"builtin", id:"mandelbrot" } } },
    { span:1, program:{ type:"formula", formula:{ type:"builtin", id:"burning_ship" } } },
  ] }),
};
const programState = Object.fromEntries(Object.keys(programs).map((name) => [name, [-0.12,0.72]]));
for (const line of programRun.stdout.trim().split("\n")) {
  const [name, iterationText, expectedReText, expectedImText] = line.split("\t");
  const program = programs[name]; const state = programState[name];
  if (!program || !state) throw new Error(`unknown program reference ${name}`);
  const next = orbit.stepLocalOrbitProgram(program, state, [-0.8,0.156], Number(iterationText)); programState[name] = next;
  const tolerance = 1e-11 * Math.max(1,Math.abs(Number(expectedReText)),Math.abs(Number(expectedImText)));
  if (Math.abs(next[0]-Number(expectedReText)) > tolerance || Math.abs(next[1]-Number(expectedImText)) > tolerance) {
    throw new Error(`${name}@${iterationText} program mismatch: browser=${next} native=${expectedReText},${expectedImText}`);
  }
}
console.log("browser/native Orbit Program parity: 36 steps passed");

const functionRun = spawnSync(reference, ["--program-functions"], { encoding:"utf8" });
if (!functionRun.stdout) throw functionRun.error ?? new Error(functionRun.stderr || "native function reference failed");
let functionCount = 0;
for (const line of functionRun.stdout.trim().split("\n")) {
  const [name, sourceText, expectedReText, expectedImText] = line.split("\t");
  const program = orbit.compileLocalOrbitProgram({ type:"formula", formula:{ type:"dsl", source:sourceText } });
  const actual = orbit.stepLocalOrbitProgram(program,[0.31,-0.27],[-0.8,0.156],0);
  const expected = [Number(expectedReText),Number(expectedImText)];
  const tolerance = 2e-11 * Math.max(1,Math.abs(expected[0]),Math.abs(expected[1]));
  if (!actual.every((value,index)=>Number.isFinite(value) && Math.abs(value-expected[index]) <= tolerance)) {
    throw new Error(`${name} function mismatch: browser=${actual} native=${expected}`);
  }
  functionCount += 1;
}
console.log(`browser/native Orbit Program functions: ${functionCount} passed`);

const parseNativeNumber = (text) => text === "inf" ? Number.POSITIVE_INFINITY
  : text === "-inf" ? Number.NEGATIVE_INFINITY : Number(text);
const closeNumeric = (actual, expected, multiplier = 2e-11) => {
  if (!Number.isFinite(expected)) return Object.is(actual, expected) || !Number.isFinite(actual);
  return Number.isFinite(actual) && Math.abs(actual - expected) <= multiplier * Math.max(1, Math.abs(expected));
};

const pairTransitionCases = {
  negative_135: { transitionFrom:"mandelbrot", transitionTo:"burning_ship", transitionThetaMilliDeg:-135000 },
  negative_45: { transitionFrom:"heart", transitionTo:"perp_buffalo", transitionThetaMilliDeg:-45000 },
  positive_37: { transitionFrom:"celtic_ship", transitionTo:"mandelceltic", transitionThetaMilliDeg:37000 },
  positive_135: { transitionFrom:"perp_ship", transitionTo:"tricorn", transitionThetaMilliDeg:135000 },
};
const cardinalTransitionCases = {
  zero: { transitionFrom:"mandelbrot", transitionTo:"burning_ship", transitionThetaMilliDeg:0 },
  positive_90: { transitionFrom:"mandelbrot", transitionTo:"burning_ship", transitionThetaMilliDeg:90000 },
  negative_90: { transitionFrom:"mandelbrot", transitionTo:"burning_ship", transitionThetaMilliDeg:-90000 },
  positive_180: { transitionFrom:"heart", transitionTo:"burning_ship", transitionThetaMilliDeg:180000 },
  negative_180: { transitionFrom:"heart", transitionTo:"burning_ship", transitionThetaMilliDeg:-180000 },
};
const multiTransitionCases = {
  two_legs: [{variant:"mandelbrot",weight:1},{variant:"burning_ship",weight:2}],
  three_legs: [{variant:"heart",weight:.5},{variant:"buffalo",weight:1.7},{variant:"mandelceltic",weight:2.8}],
  skip_nonpositive: [{variant:"tricorn",weight:0},{variant:"celtic",weight:3},{variant:"perp_ship",weight:-2},{variant:"celtic_ship",weight:1}],
};
const transitionRun = spawnSync(reference, ["--transition"], { encoding:"utf8" });
if (!transitionRun.stdout) throw transitionRun.error ?? new Error(transitionRun.stderr || "native transition reference failed");
let transitionCount = 0; let thetaCount = 0;
for (const line of transitionRun.stdout.trim().split("\n")) {
  const columns = line.split("\t");
  if (columns[0] === "theta") {
    const actual = transition.normalizeLocalTransitionMilliDeg(Number(columns[1]));
    if (actual !== Number(columns[2])) throw new Error(`theta ${columns[1]} normalized to ${actual}, expected ${columns[2]}`);
    thetaCount += 1; continue;
  }
  const [kind,name,uText,vText,juliaText,iterText,normText,escapedText,minText,maxText,envelopeText,pairwiseText] = columns;
  const pair = kind === "pair" ? pairTransitionCases[name] : kind === "cardinal" ? cardinalTransitionCases[name] : null;
  const legs = kind === "multi" ? multiTransitionCases[name] : undefined;
  if ((!pair && !legs) || !name) throw new Error(`unknown transition reference ${line}`);
  const common = {
    centerRe:0,centerIm:0,scale:3,iterations:90,metric:"escape",bailout:2,rotationDeg:0,
    julia:juliaText === "1",juliaRe:-.8,juliaIm:.156,transitionThetaMilliDeg:pair?.transitionThetaMilliDeg ?? 0,
    transitionFrom:pair?.transitionFrom ?? "mandelbrot",transitionTo:pair?.transitionTo ?? "burning_ship",
    transitionLegs:legs,pairwiseCap:11,
  };
  const u=Number(uText),v=Number(vText);
  const escape=transition.iterateLocalTransitionPoint(common,u,v);
  const minimum=transition.iterateLocalTransitionPoint({...common,metric:"min_abs"},u,v);
  const maximum=transition.iterateLocalTransitionPoint({...common,metric:"max_abs"},u,v);
  const envelope=transition.iterateLocalTransitionPoint({...common,metric:"envelope"},u,v);
  const pairwise=transition.iterateLocalTransitionPoint({...common,metric:"min_pairwise_dist"},u,v);
  const expected=[parseNativeNumber(normText),parseNativeNumber(minText),parseNativeNumber(maxText),parseNativeNumber(envelopeText),parseNativeNumber(pairwiseText)];
  if (escape.iter !== Number(iterText) || escape.escaped !== (escapedText === "1") || !closeNumeric(escape.norm,expected[0])
    || !closeNumeric(minimum.field,expected[1]) || !closeNumeric(maximum.field,expected[2])
    || !closeNumeric(envelope.field,expected[3]) || !closeNumeric(pairwise.field,expected[4])) {
    throw new Error(`${kind}/${name} transition mismatch: browser=${JSON.stringify({escape,minimum,maximum,envelope,pairwise})} native=${columns.slice(5).join("/")}`);
  }
  transitionCount += 1;
}
console.log(`browser/native transition parity: ${transitionCount} point sets + ${thetaCount} normalized angles passed`);

const agreementRun = spawnSync(reference, ["--agreement"], { encoding:"utf8" });
if (!agreementRun.stdout) throw agreementRun.error ?? new Error(agreementRun.stderr || "native agreement reference failed");
let agreementCount=0;
for(const line of agreementRun.stdout.trim().split("\n")){
  const [variant,reText,imText,juliaText,iterText,fullyText]=line.split("\t");
  const actual=transition.iterateLocalMandelShipAgreementPoint({centerRe:0,centerIm:0,scale:3,iterations:100,
    variant,bailout:2,julia:juliaText==="1",juliaRe:-.8,juliaIm:.156},Number(reText),Number(imText));
  if(actual.iter!==Number(iterText)||actual.fullyAgrees!==(fullyText==="1"))throw new Error(`${variant} agreement mismatch: browser=${JSON.stringify(actual)} native=${iterText}/${fullyText}`);
  agreementCount+=1;
}
console.log(`browser/native mandel_ship_agree parity: ${agreementCount} samples passed`);

const colorRun=spawnSync(reference,["--color-program"],{encoding:"utf8"});
if(!colorRun.stdout)throw colorRun.error??new Error(colorRun.stderr||"native color program reference failed");
const colorPrograms=Object.fromEntries(["clamp","repeat","mirror"].map((wrap)=>[wrap,cache.compileColorProgram({schemaVersion:1,type:"gradient",wrap,cycles:2.5,phase:-.25,
  interiorColor:"#070b13",invalidColor:"#ff00ff",stops:[{at:0,color:"#040810"},{at:.27,color:"#3366cc"},{at:.73,color:"#f05023"},{at:1,color:"#faf5dc"}]})]));
let colorCount=0;
for(const line of colorRun.stdout.trim().split("\n")){
  const [wrap,inputText,rText,gText,bText]=line.split("\t");const program=colorPrograms[wrap];let actual;
  if(inputText==="interior")actual=cache.colorizeRawField({kind:"escape",metric:"escape",width:1,height:1,bailout:2,iterationLimit:10,iterations:new Uint32Array([10]),norms:new Float64Array([0])},program);
  else if(inputText==="invalid")actual=cache.colorizeRawField({kind:"metric",metric:"min_abs",width:1,height:1,bailout:1,values:new Float64Array([Number.NaN])},program);
  else actual=cache.colorizeRawField({kind:"metric",metric:"min_abs",width:1,height:1,bailout:1,values:new Float64Array([Number(inputText)])},program);
  if(actual[0]!==Number(rText)||actual[1]!==Number(gText)||actual[2]!==Number(bText))throw new Error(`${wrap}/${inputText} color mismatch: browser=${actual.slice(0,3)} native=${rText},${gText},${bText}`);
  colorCount+=1;
}
console.log(`browser/native ColorProgram parity: ${colorCount} cases passed`);

const cacheKeyA=cache.createRawFieldCacheKey({centerRe:-.75,metric:"escape",colorMap:"viridis",smooth:false},32,24);
const cacheKeyB=cache.createRawFieldCacheKey({smooth:true,metric:"escape",centerRe:-.75,colorMap:"inferno",colorProgram:{anything:"ignored"}},32,24);
if(cacheKeyA!==cacheKeyB)throw new Error("palette-only edits changed raw-field cache key");
const lru=new cache.RawFieldCache(16);
const tiny=(value)=>({kind:"metric",metric:"min_abs",width:1,height:1,bailout:2,values:new Float64Array([value])});
lru.set("a",tiny(1));lru.set("b",tiny(2));lru.get("a");lru.set("c",tiny(3));
if(!lru.has("a")||lru.has("b")||!lru.has("c"))throw new Error("raw-field LRU ordering mismatch");
for(const invalidProgram of [
  {type:"formula",formula:{type:"dsl",source:"z+unknown"}},
  {type:"formula",formula:{type:"dsl",source:"toString()"}},
  {type:"formula",formula:{type:"dsl",source:"z".repeat(4097)}},
  {type:"sequence",repeat:false,steps:[{span:1,program:{type:"formula",formula:{type:"builtin",id:"mandelbrot"}}}]},
]) { let rejected=false;try{orbit.compileLocalOrbitProgram(invalidProgram);}catch{rejected=true;}if(!rejected)throw new Error("invalid Orbit Program was accepted"); }
console.log("browser local cache + safety limits: passed");
