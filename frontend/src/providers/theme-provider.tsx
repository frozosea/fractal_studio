"use client";

import * as React from "react";
import {
  DARK_MEDIA_QUERY,
  THEME_STORAGE_KEY,
  isTheme,
  resolveTheme,
  type ResolvedTheme,
  type Theme,
} from "@/lib/theme";

interface ThemeContextValue {
  /** What the user picked — may be "system". */
  theme: Theme;
  /** What that resolves to right now. */
  resolvedTheme: ResolvedTheme;
  setTheme: (theme: Theme) => void;
  /**
   * False until the stored preference has been read on the client. Controls
   * that render differently per theme must wait for it, or the server markup
   * (which cannot know the preference) will not match on hydration.
   */
  ready: boolean;
}

const ThemeContext = React.createContext<ThemeContextValue | null>(null);

function systemPrefersDark(): boolean {
  return window.matchMedia(DARK_MEDIA_QUERY).matches;
}

function readStoredTheme(): Theme {
  try {
    const stored = window.localStorage.getItem(THEME_STORAGE_KEY);
    if (isTheme(stored)) return stored;
  } catch {
    // Private-mode Safari throws rather than returning null.
  }
  return "system";
}

function applyTheme(resolved: ResolvedTheme) {
  const root = document.documentElement;
  root.classList.toggle("dark", resolved === "dark");
  // Keeps native form controls, scrollbars and the caret in step; CSS alone
  // cannot restyle them.
  root.style.colorScheme = resolved;
}

export function ThemeProvider({ children }: { children: React.ReactNode }) {
  const [theme, setThemeState] = React.useState<Theme>("system");
  const [resolvedTheme, setResolvedTheme] = React.useState<ResolvedTheme>("dark");
  const [ready, setReady] = React.useState(false);

  // THEME_INIT_SCRIPT has already put the right class on <html>; this only
  // tells React what it chose, so no re-apply is needed here.
  React.useEffect(() => {
    const stored = readStoredTheme();
    setThemeState(stored);
    setResolvedTheme(resolveTheme(stored, systemPrefersDark()));
    setReady(true);
  }, []);

  // Track the OS while the preference is "system" — someone flipping their
  // desktop to dark at sunset should see the app follow without a reload.
  React.useEffect(() => {
    if (theme !== "system") return;
    const media = window.matchMedia(DARK_MEDIA_QUERY);
    const onChange = () => {
      const next: ResolvedTheme = media.matches ? "dark" : "light";
      setResolvedTheme(next);
      applyTheme(next);
    };
    media.addEventListener("change", onChange);
    return () => media.removeEventListener("change", onChange);
  }, [theme]);

  const setTheme = React.useCallback((next: Theme) => {
    setThemeState(next);
    try {
      window.localStorage.setItem(THEME_STORAGE_KEY, next);
    } catch {
      // A rejected write costs persistence, not the switch itself.
    }
    const resolved = resolveTheme(next, systemPrefersDark());
    setResolvedTheme(resolved);
    applyTheme(resolved);
  }, []);

  const value = React.useMemo<ThemeContextValue>(
    () => ({ theme, resolvedTheme, setTheme, ready }),
    [theme, resolvedTheme, setTheme, ready],
  );

  return <ThemeContext.Provider value={value}>{children}</ThemeContext.Provider>;
}

export function useTheme(): ThemeContextValue {
  const context = React.useContext(ThemeContext);
  if (!context) throw new Error("useTheme must be used within a ThemeProvider");
  return context;
}
