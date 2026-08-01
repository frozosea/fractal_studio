/**
 * Theme constants shared by the blocking init script and the React provider.
 *
 * Kept free of "use client" so the server layout can inline the script without
 * pulling the provider into the server bundle.
 */

export type Theme = "light" | "dark" | "system";
export type ResolvedTheme = "light" | "dark";

export const THEME_STORAGE_KEY = "fractal-studio-theme";

export const DARK_MEDIA_QUERY = "(prefers-color-scheme: dark)";

export function isTheme(value: unknown): value is Theme {
  return value === "light" || value === "dark" || value === "system";
}

/**
 * The one rule both the init script and the provider follow: an explicit
 * choice wins, anything else follows the OS. Corrupt storage therefore behaves
 * like "system" rather than silently pinning the light theme.
 */
export function resolveTheme(stored: string | null, systemPrefersDark: boolean): ResolvedTheme {
  if (stored === "dark") return "dark";
  if (stored === "light") return "light";
  return systemPrefersDark ? "dark" : "light";
}

/**
 * Runs before first paint, straight from <head>.
 *
 * Without it the document paints with whatever the server sent and then jumps
 * when the provider mounts — a full-screen flash of the wrong theme on every
 * cold load. It must therefore stay synchronous, tiny, and independent of the
 * React bundle, which is why the rule above is duplicated here rather than
 * imported.
 *
 * The `try` covers only the storage read: Safari's private mode throws on
 * `getItem` outright, and swallowing the whole body there would drop the OS
 * preference too and hand those users the light theme on a dark desktop.
 */
export const THEME_INIT_SCRIPT = `(function(){var s=null;try{s=localStorage.getItem(${JSON.stringify(
  THEME_STORAGE_KEY,
)});}catch(_){}var d=s==="dark"||(s!=="light"&&window.matchMedia(${JSON.stringify(
  DARK_MEDIA_QUERY,
)}).matches),e=document.documentElement;e.classList.toggle("dark",d);e.style.colorScheme=d?"dark":"light";})();`;
