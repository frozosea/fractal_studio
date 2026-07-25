/**
 * Format elapsed time in milliseconds to a human-readable string.
 * Examples: "1.2s", "45ms", "3m 12s", "1h 5m"
 */
export function formatMs(ms: number): string {
  if (!Number.isFinite(ms) || ms < 0) return '—';

  if (ms < 1_000) {
    return `${Math.round(ms)}ms`;
  }

  if (ms < 60_000) {
    const sec = ms / 1_000;
    return sec < 10 ? `${sec.toFixed(1)}s` : `${Math.round(sec)}s`;
  }

  const minutes = Math.floor(ms / 60_000);
  const seconds = Math.round((ms % 60_000) / 1_000);

  if (minutes < 60) {
    return seconds > 0 ? `${minutes}m ${seconds}s` : `${minutes}m`;
  }

  const hours = Math.floor(minutes / 60);
  const remainingMinutes = minutes % 60;
  return remainingMinutes > 0
    ? `${hours}h ${remainingMinutes}m`
    : `${hours}h`;
}

/**
 * Format bytes to a human-readable string (KiB, MiB, GiB).
 */
export function formatBytes(bytes: number): string {
  if (!Number.isFinite(bytes) || bytes < 0) return '—';

  if (bytes === 0) return '0 B';

  const units = ['B', 'KiB', 'MiB', 'GiB', 'TiB'];
  const base = 1024;
  const magnitude = Math.min(
    Math.floor(Math.log(bytes) / Math.log(base)),
    units.length - 1,
  );

  const value = bytes / Math.pow(base, magnitude);
  const formatted =
    magnitude === 0
      ? String(Math.round(value))
      : value < 10
        ? value.toFixed(1)
        : Math.round(value).toLocaleString();

  return `${formatted} ${units[magnitude]}`;
}

/**
 * Format an ISO date string to a localized date + time.
 * Falls back to the raw string if parsing fails.
 */
export function formatDate(iso: string): string {
  if (!iso) return '—';

  try {
    const date = new Date(iso);
    if (isNaN(date.getTime())) return iso;

    return date.toLocaleString('zh-CN', {
      year: 'numeric',
      month: '2-digit',
      day: '2-digit',
      hour: '2-digit',
      minute: '2-digit',
      second: '2-digit',
      hour12: false,
    });
  } catch {
    return iso;
  }
}

/**
 * Format a percentage value (0–100) with one decimal place.
 */
export function formatPercent(pct: number): string {
  if (!Number.isFinite(pct)) return '—';
  if (pct >= 99.95) return '100%';
  if (pct <= 0.05) return '0%';
  return `${pct.toFixed(1)}%`;
}

/**
 * Format a number with locale-aware separators and optional decimal places.
 */
export function formatNumber(n: number, decimals?: number): string {
  if (!Number.isFinite(n)) return '—';

  if (decimals !== undefined) {
    return n.toLocaleString('zh-CN', {
      minimumFractionDigits: decimals,
      maximumFractionDigits: decimals,
    });
  }

  // Auto-detect: show up to 3 significant decimals, strip trailing zeros.
  if (Number.isInteger(n)) {
    return n.toLocaleString('zh-CN');
  }

  const abs = Math.abs(n);
  if (abs >= 1_000) return Math.round(n).toLocaleString('zh-CN');
  if (abs >= 1) return n.toLocaleString('zh-CN', { maximumFractionDigits: 4 });
  if (abs >= 0.01) return n.toLocaleString('zh-CN', { maximumFractionDigits: 6 });
  return n.toExponential(2);
}
