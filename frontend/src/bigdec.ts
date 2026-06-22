// bigdec.ts — Arbitrary-precision decimal arithmetic for deep-zoom coordinates.
//
// Stores numbers as { mantissa: bigint, exp: number } where value = mantissa * 10^exp.
// Uses JS BigInt for exact integer arithmetic — no external library needed.

export interface BigDec {
  mantissa: bigint
  exp: number      // power of 10 (negative = fractional digits)
}

const ZERO: BigDec = { mantissa: 0n, exp: 0 }

export function bdZero(): BigDec { return { ...ZERO } }

export function bdFromString(s: string): BigDec {
  s = s.trim()
  if (s === '' || s === '0') return bdZero()

  const neg = s.startsWith('-')
  if (neg || s.startsWith('+')) s = s.slice(1)

  // Handle scientific notation: "1.23e-5"
  let eIdx = s.indexOf('e')
  if (eIdx < 0) eIdx = s.indexOf('E')
  let sciExp = 0
  if (eIdx >= 0) {
    sciExp = parseInt(s.slice(eIdx + 1), 10)
    s = s.slice(0, eIdx)
  }

  const dot = s.indexOf('.')
  let fracDigits = 0
  if (dot >= 0) {
    fracDigits = s.length - dot - 1
    s = s.slice(0, dot) + s.slice(dot + 1)
  }

  // Remove leading zeros (but keep at least one digit)
  s = s.replace(/^0+(?=\d)/, '')
  if (s === '') return bdZero()

  let mantissa = BigInt(s)
  if (neg) mantissa = -mantissa
  const exp = -fracDigits + sciExp

  return bdNormalize({ mantissa, exp })
}

export function bdFromNumber(n: number): BigDec {
  if (n === 0 || !Number.isFinite(n)) return bdZero()
  // Convert via string to preserve all significant digits
  return bdFromString(n.toPrecision(17))
}

function bdNormalize(a: BigDec): BigDec {
  if (a.mantissa === 0n) return bdZero()
  let { mantissa, exp } = a
  // Strip trailing zeros from mantissa
  while (mantissa !== 0n && mantissa % 10n === 0n) {
    mantissa /= 10n
    exp++
  }
  return { mantissa, exp }
}

function alignExp(a: BigDec, b: BigDec): [bigint, bigint, number] {
  const minExp = Math.min(a.exp, b.exp)
  const am = a.mantissa * 10n ** BigInt(a.exp - minExp)
  const bm = b.mantissa * 10n ** BigInt(b.exp - minExp)
  return [am, bm, minExp]
}

export function bdAdd(a: BigDec, b: BigDec): BigDec {
  const [am, bm, exp] = alignExp(a, b)
  return bdNormalize({ mantissa: am + bm, exp })
}

export function bdAddNumber(a: BigDec, n: number): BigDec {
  if (n === 0) return a
  return bdAdd(a, bdFromNumber(n))
}

export function bdToString(a: BigDec): string {
  if (a.mantissa === 0n) return '0'

  const neg = a.mantissa < 0n
  let digits = (neg ? -a.mantissa : a.mantissa).toString()

  if (a.exp >= 0) {
    // Integer: append zeros
    return (neg ? '-' : '') + digits + '0'.repeat(a.exp)
  }

  // Fractional: insert decimal point
  const fracCount = -a.exp
  if (fracCount >= digits.length) {
    // Need leading zeros: e.g. mantissa=5, exp=-3 → "0.005"
    digits = '0'.repeat(fracCount - digits.length + 1) + digits
  }
  const intPart = digits.slice(0, digits.length - fracCount)
  const fracPart = digits.slice(digits.length - fracCount)
  const s = (intPart || '0') + '.' + fracPart
  return (neg ? '-' : '') + s
}

export function bdToNumber(a: BigDec): number {
  return parseFloat(bdToString(a))
}
