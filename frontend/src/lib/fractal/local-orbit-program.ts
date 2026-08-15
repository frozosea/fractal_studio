import type { OrbitProgram } from "@/lib/api/platform";
import type { LocalVariant } from "./local-render-core";

export type ComplexValue = readonly [re: number, im: number];

type TokenKind = "end" | "number" | "identifier" | "+" | "-" | "*" | "/" | "^" | "(" | ")" | ",";
type Token = { kind: TokenKind; text: string; number: number; position: number };
type Expression =
  | { kind: "number"; value: ComplexValue }
  | { kind: "variable"; name: string }
  | { kind: "unary"; negative: boolean; child: Expression }
  | { kind: "binary"; operator: "+" | "-" | "*" | "/" | "^"; left: Expression; right: Expression }
  | { kind: "call"; name: FunctionName; arguments: Expression[] };

type FunctionName = "sin" | "cos" | "tan" | "exp" | "log" | "pow" | "sqrt" | "abs" | "conj" | "sinh" | "cosh" | "tanh" | "real" | "imag";
type CompiledFormula =
  | { kind: "builtin"; variant: LocalVariant; nodeCount: 1; depth: 1 }
  | { kind: "dsl"; expression: Expression; parameters: Record<string, ComplexValue>; nodeCount: number; depth: number };

export type CompiledLocalOrbitProgram =
  | { kind: "formula"; formula: CompiledFormula; certifiedRadius: number | null }
  | { kind: "sequence"; steps: Array<{ span: number; formula: CompiledFormula }>; cycleSpan: number; certifiedRadius: number | null };

const BUILTIN_VARIANTS = new Set<string>([
  "mandelbrot", "tricorn", "burning_ship", "celtic", "heart", "buffalo", "perp_buffalo",
  "celtic_ship", "mandelceltic", "perp_ship", "sin_z", "cos_z", "exp_z", "sinh_z", "cosh_z", "tan_z",
]);
const CERTIFIED_VARIANTS = new Set<string>(["mandelbrot", "tricorn", "burning_ship", "celtic", "heart", "buffalo", "perp_buffalo", "celtic_ship", "mandelceltic", "perp_ship"]);
const FUNCTIONS: Record<FunctionName, number> = { sin:1, cos:1, tan:1, exp:1, log:1, pow:2, sqrt:1, abs:1, conj:1, sinh:1, cosh:1, tanh:1, real:1, imag:1 };
const VARIABLES = new Set(["z", "c", "n", "i", "pi", "e"]);

class FormulaError extends Error {}

class Lexer {
  private position = 0;
  constructor(private readonly source: string) {}

  next(): Token {
    while (/\s/.test(this.source[this.position] ?? "")) this.position += 1;
    const start = this.position; const character = this.source[this.position];
    if (character === undefined) return { kind: "end", text: "", number: 0, position: start };
    if (["+", "-", "*", "/", "^", "(", ")", ","].includes(character)) {
      this.position += 1; return { kind: character as TokenKind, text: character, number: 0, position: start };
    }
    const number = this.source.slice(start).match(/^(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?/);
    if (number) {
      this.position += number[0].length; const value = Number(number[0]);
      if (!Number.isFinite(value)) throw new FormulaError(`invalid number at ${start}`);
      return { kind: "number", text: number[0], number: value, position: start };
    }
    const identifier = this.source.slice(start).match(/^[A-Za-z_][A-Za-z0-9_]*/);
    if (identifier) {
      this.position += identifier[0].length;
      return { kind: "identifier", text: identifier[0], number: 0, position: start };
    }
    throw new FormulaError(`invalid character at ${start}`);
  }
}

class Parser {
  private current: Token;
  private nodes = 0;
  private maximumDepth = 1;
  constructor(private readonly lexer: Lexer, private readonly parameters: Set<string>) { this.current = lexer.next(); }

  parse(): Expression {
    const result = this.expression(0, 1);
    if (this.current.kind !== "end") throw new FormulaError(`unexpected token at ${this.current.position}`);
    return result;
  }
  get nodeCount() { return this.nodes; }
  get depth() { return this.maximumDepth; }

  private count<T extends Expression>(node: T, depth: number): T {
    if (depth > 32) throw new FormulaError("formula nesting exceeds 32 levels");
    this.maximumDepth = Math.max(this.maximumDepth, depth);
    this.nodes += 1; if (this.nodes > 256) throw new FormulaError("formula exceeds 256 nodes");
    return node;
  }

  private advance() { const token = this.current; this.current = this.lexer.next(); return token; }
  private at(kind: TokenKind) { return this.current.kind === kind; }
  private binding(kind: TokenKind) { return kind === "+" || kind === "-" ? 10 : kind === "*" || kind === "/" ? 20 : kind === "^" ? 30 : 0; }

  private expression(rightBinding: number, depth: number): Expression {
    let left = this.prefix(this.advance(), depth);
    while (rightBinding < this.binding(this.current.kind)) {
      const operator = this.advance() as Token & { kind: "+" | "-" | "*" | "/" | "^" };
      const binding = this.binding(operator.kind);
      left = this.count({ kind: "binary", operator: operator.kind, left, right: this.expression(operator.kind === "^" ? binding - 1 : binding, depth + 1) }, depth);
    }
    return left;
  }

  private prefix(token: Token, depth: number): Expression {
    if (token.kind === "number") return this.count({ kind: "number", value: [token.number, 0] }, depth);
    if (token.kind === "+" || token.kind === "-") return this.count({ kind: "unary", negative: token.kind === "-", child: this.expression(25, depth + 1) }, depth);
    if (token.kind === "(") {
      const result = this.expression(0, depth + 1); if (this.current.kind !== ")") throw new FormulaError(`expected ')' at ${this.current.position}`); this.advance(); return result;
    }
    if (token.kind !== "identifier") throw new FormulaError(`expected expression at ${token.position}`);
    if (this.current.kind !== "(") {
      if (!VARIABLES.has(token.text) && !this.parameters.has(token.text)) throw new FormulaError(`unknown identifier ${token.text}`);
      return this.count({ kind: "variable", name: token.text }, depth);
    }
    if (!Object.hasOwn(FUNCTIONS, token.text)) throw new FormulaError(`unknown function ${token.text}`);
    this.advance(); const arguments_: Expression[] = [];
    if (!this.at(")")) for (;;) {
      arguments_.push(this.expression(0, depth + 1)); if (!this.at(",")) break; this.advance();
    }
    if (!this.at(")")) throw new FormulaError(`expected ')' at ${this.current.position}`); this.advance();
    const name = token.text as FunctionName;
    if (arguments_.length !== FUNCTIONS[name]) throw new FormulaError(`${name} expects ${FUNCTIONS[name]} arguments`);
    return this.count({ kind: "call", name, arguments: arguments_ }, depth);
  }
}

const add = (a: ComplexValue, b: ComplexValue): ComplexValue => [a[0]+b[0],a[1]+b[1]];
const sub = (a: ComplexValue, b: ComplexValue): ComplexValue => [a[0]-b[0],a[1]-b[1]];
const mul = (a: ComplexValue, b: ComplexValue): ComplexValue => [a[0]*b[0]-a[1]*b[1],a[0]*b[1]+a[1]*b[0]];
const div = (a: ComplexValue, b: ComplexValue): ComplexValue => { const d=b[0]*b[0]+b[1]*b[1]; return [(a[0]*b[0]+a[1]*b[1])/d,(a[1]*b[0]-a[0]*b[1])/d]; };
const exp = (z: ComplexValue): ComplexValue => { const value=Math.exp(z[0]); return [value*Math.cos(z[1]),value*Math.sin(z[1])]; };
const log = (z: ComplexValue): ComplexValue => [Math.log(Math.hypot(z[0],z[1])),Math.atan2(z[1],z[0])];
const sqrt = (z: ComplexValue): ComplexValue => { const magnitude=Math.hypot(z[0],z[1]); return [Math.sqrt(Math.max(0,(magnitude+z[0])/2)),Math.sign(z[1] || 1)*Math.sqrt(Math.max(0,(magnitude-z[0])/2))]; };
const sin = (z: ComplexValue): ComplexValue => [Math.sin(z[0])*Math.cosh(z[1]),Math.cos(z[0])*Math.sinh(z[1])];
const cos = (z: ComplexValue): ComplexValue => [Math.cos(z[0])*Math.cosh(z[1]),-Math.sin(z[0])*Math.sinh(z[1])];
const sinh = (z: ComplexValue): ComplexValue => [Math.sinh(z[0])*Math.cos(z[1]),Math.cosh(z[0])*Math.sin(z[1])];
const cosh = (z: ComplexValue): ComplexValue => [Math.cosh(z[0])*Math.cos(z[1]),Math.sinh(z[0])*Math.sin(z[1])];

function builtinStep(variant: LocalVariant, z: ComplexValue, c: ComplexValue): ComplexValue {
  const [x,y]=z; const x2=x*x; const y2=y*y; const xy=2*x*y;
  switch (variant) {
    case "mandelbrot": return [x2-y2+c[0],xy+c[1]]; case "tricorn": return [x2-y2+c[0],-xy+c[1]];
    case "burning_ship": return [x2-y2+c[0],2*Math.abs(x)*Math.abs(y)+c[1]]; case "celtic": return [x2-y2+c[0],2*x*Math.abs(y)+c[1]];
    case "heart": return [x2-y2+c[0],-2*Math.abs(x)*y+c[1]]; case "buffalo": return [Math.abs(x2-y2)+c[0],xy+c[1]];
    case "perp_buffalo": return [Math.abs(x2-y2)+c[0],-xy+c[1]]; case "celtic_ship": return [Math.abs(x2-y2)+c[0],Math.abs(xy)+c[1]];
    case "mandelceltic": return [Math.abs(x2-y2)+c[0],2*x*Math.abs(y)+c[1]]; case "perp_ship": return [Math.abs(x2-y2)+c[0],-2*Math.abs(x)*y+c[1]];
    case "sin_z": return add(sin(z),c); case "cos_z": return add(cos(z),c); case "exp_z": return add(exp(z),c);
    case "sinh_z": return add(sinh(z),c); case "cosh_z": return add(cosh(z),c);
    case "tan_z": { const denominator=Math.cos(2*x)+Math.cosh(2*y); return denominator===0 ? c : [Math.sin(2*x)/denominator+c[0],Math.sinh(2*y)/denominator+c[1]]; }
  }
}

function evaluate(expression: Expression, variables: Record<string, ComplexValue>): ComplexValue {
  if (expression.kind === "number") return expression.value;
  if (expression.kind === "variable") return variables[expression.name] ?? [Number.NaN,Number.NaN];
  if (expression.kind === "unary") { const value=evaluate(expression.child,variables); return expression.negative ? [-value[0],-value[1]] : value; }
  if (expression.kind === "binary") {
    const left=evaluate(expression.left,variables); const right=evaluate(expression.right,variables);
    if (expression.operator === "+") return add(left,right); if (expression.operator === "-") return sub(left,right);
    if (expression.operator === "*") return mul(left,right); if (expression.operator === "/") return div(left,right);
    return exp(mul(right,log(left)));
  }
  const args=expression.arguments.map((argument)=>evaluate(argument,variables)); const first=args[0] ?? [Number.NaN,Number.NaN] as const;
  switch (expression.name) {
    case "sin": return sin(first); case "cos": return cos(first); case "tan": return div(sin(first),cos(first)); case "exp": return exp(first);
    case "log": return log(first); case "pow": return exp(mul(args[1] ?? [Number.NaN,Number.NaN],log(first))); case "sqrt": return sqrt(first);
    case "abs": return [Math.hypot(first[0],first[1]),0]; case "conj": return [first[0],-first[1]]; case "sinh": return sinh(first);
    case "cosh": return cosh(first); case "tanh": return div(sinh(first),cosh(first)); case "real": return [first[0],0]; case "imag": return [first[1],0];
  }
}

function compileFormula(input: Extract<OrbitProgram,{type:"formula"}>["formula"]): CompiledFormula {
  if (input.type === "builtin") {
    if (!BUILTIN_VARIANTS.has(input.id)) throw new FormulaError(`unknown builtin ${input.id}`);
    return { kind:"builtin",variant:input.id as LocalVariant,nodeCount:1,depth:1 };
  }
  if (!input.source || input.source.length > 4096) throw new FormulaError("formula source must contain 1..4096 bytes");
  const entries=Object.entries(input.parameters ?? {}).sort(([left],[right])=>left.localeCompare(right));
  if (entries.length > 16) throw new FormulaError("formula has more than 16 parameters");
  const parameters: Record<string,ComplexValue> = {};
  for (const [name,value] of entries) {
    if (!/^[A-Za-z][A-Za-z0-9_]*$/.test(name) || VARIABLES.has(name)) throw new FormulaError(`invalid parameter ${name}`);
    const complex: ComplexValue=typeof value === "number" ? [value,0] : [value.re,value.im];
    if (!Number.isFinite(complex[0]) || !Number.isFinite(complex[1])) throw new FormulaError(`non-finite parameter ${name}`);
    parameters[name]=complex;
  }
  const parser = new Parser(new Lexer(input.source),new Set(Object.keys(parameters)));
  const expression = parser.parse();
  return { kind:"dsl",expression,parameters,nodeCount:parser.nodeCount,depth:parser.depth };
}

export function compileLocalOrbitProgram(input: OrbitProgram): CompiledLocalOrbitProgram {
  if (input.type === "formula") {
    const formula=compileFormula(input.formula); const certified=formula.kind === "builtin" && CERTIFIED_VARIANTS.has(formula.variant);
    return { kind:"formula",formula,certifiedRadius:certified ? 2 : null };
  }
  if (!input.repeat || input.steps.length < 1 || input.steps.length > 64) throw new FormulaError("sequence needs 1..64 repeating steps");
  let cycleSpan=0; let certified=true; let totalNodes=1;
  const steps=input.steps.map((step)=>{
    if (!Number.isInteger(step.span) || step.span < 1 || step.span > 1_000_000 || cycleSpan > 1_000_000-step.span) throw new FormulaError("invalid sequence span");
    cycleSpan+=step.span; const formula=compileFormula(step.program.formula); certified &&= formula.kind === "builtin" && CERTIFIED_VARIANTS.has(formula.variant);
    totalNodes += formula.nodeCount;
    if (totalNodes > 256 || formula.depth + 1 > 32) throw new FormulaError("sequence exceeds formula complexity limits");
    return { span:step.span,formula };
  });
  return { kind:"sequence",steps,cycleSpan,certifiedRadius:certified ? 2 : null };
}

function evaluateFormula(formula: CompiledFormula, z: ComplexValue, c: ComplexValue, iteration: number): ComplexValue {
  if (formula.kind === "builtin") return builtinStep(formula.variant,z,c);
  return evaluate(formula.expression,{ ...formula.parameters,z,c,n:[iteration,0],i:[0,1],pi:[Math.PI,0],e:[Math.E,0] });
}

export function stepLocalOrbitProgram(program: CompiledLocalOrbitProgram, z: ComplexValue, c: ComplexValue, iteration: number): ComplexValue {
  if (program.kind === "formula") return evaluateFormula(program.formula,z,c,iteration);
  let offset=((iteration%program.cycleSpan)+program.cycleSpan)%program.cycleSpan;
  for (const step of program.steps) { if (offset < step.span) return evaluateFormula(step.formula,z,c,iteration); offset-=step.span; }
  return [Number.NaN,Number.NaN];
}
